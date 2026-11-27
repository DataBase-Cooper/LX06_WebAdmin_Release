/* lx06_unified.c —— SSH(22, dropbear) + HTTP(88, webd) 单二进制集成入口。
 *
 * 设计：dropbear 的 svr_main 仍是主进程（管 22 端口，procd 保活）。
 * 在 svr-main.c 的 listen+pidfile 就绪后，调用 lx06_unified_http_start()
 * fork 一个子进程 exec webd_main()（HTTP 88）。webd 由主进程以 SIG_IGN
 * 方式带（主进程已 SIG_IGN SIGCHLD）；webd 挂掉则不自动重拉（procd 重启
 * 整个 lx06unified 即可恢复，SSH 期间不受影响）。
 *
 * 编译标志：-DLX06_UNIFIED（options.h 控制 svr-main 的入口名）。
 */
#include "includes.h"
#include "dbutil.h"
#include "session.h"
#include "buffer.h"
#include "runopts.h"
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h> /* flock（单实例护栏） */

/* webd 入口（webd.c 的 main 改名而来） */
int webd_main(void);

#ifndef LX06_WEBD_PORT
#define LX06_WEBD_PORT 88
#endif

/* fix15 日志开关读取（conf /data/lx06-ssh.conf）：缺省=开，显式 0 才关。
 * 整文件一次读入（conf 是数行 key=value，<4K），逐行前缀匹配 key=。 */
static int lx06_log_conf_on(const char *key)
{
	char path[64], k[40];
	char buf[2048];
	int fd;
	int on = 1;
	long got = 0, i;

	snprintf(k, sizeof(k), "%s=", key);
	snprintf(path, sizeof(path), "/data/lx06-ssh.conf");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 1;
	while (got < (long)sizeof(buf) - 1) {
		ssize_t n = read(fd, buf + got, (size_t)(sizeof(buf) - 1 - got));
		if (n <= 0)
			break;
		got += n;
	}
	close(fd);
	buf[got] = 0;
	/* 逐行匹配：行首 == "key=" 且下一字符是值 */
	i = 0;
	while (i <= got) {
		long j;
		long len;
		for (j = i; j < got; j++)
			if (buf[j] == '\n')
				break;
		len = j - i;
		if (len > (long)strlen(k) &&
		    strncmp(buf + i, k, strlen(k)) == 0) {
			if (buf[i + strlen(k)] == '0')
				on = 0;
		}
		if (j >= got)
			break;
		i = j + 1;
	}
	return on;
}

/* 原地截尾保留最后 64K（inode 不变，O_APPEND 持 fd 写者续写安全）。
 * 与 webd.c 的 log_rotate 同语义（两半都受 64K 硬上限）。 */
static void lx06_log_trunc_keep64k(const char *p)
{
	long sz, skip, tail, off, rem;
	char b[4096];
	int fd;
	char rot[70];

	fd = open(p, O_RDONLY);
	if (fd < 0)
		return;
	{
		off_t e = lseek(fd, 0, SEEK_END);
		if (e < 0) { close(fd); return; }
		sz = (long)e;
	}
	close(fd);
	if (sz <= 65536)
		return;
	skip = sz - 65536;
	tail = 65536;
	/* 旧段尾部 64K → .1（.1 也硬限 64K） */
	{
		long start = skip - 65536;
		if (start < 0)
			start = 0;
		int rfd;
		fd = open(p, O_RDONLY);
		if (fd < 0)
			return;
		rfd = open(snprintf(rot, sizeof(rot), "%s.1", p) ? rot : "x",
		    O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (rfd >= 0) {
			rem = skip - start;
			if (lseek(fd, start, SEEK_SET) == start) {
				while (rem > 0) {
					long n = read(fd, b, (rem > 4096) ? 4096 : rem);
					long w;
					if (n <= 0)
						break;
					w = 0;
					while (w < n) {
						long m = write(rfd, b + w, n - w);
						if (m <= 0)
							break;
						w += m;
					}
					rem -= n;
				}
			}
			close(rfd);
		}
		close(fd);
	}
	/* 尾部原地下移到文件头 + truncate 64K */
	fd = open(p, O_RDWR);
	if (fd < 0)
		return;
	off = 0;
	rem = tail;
	while (rem > 0) {
		long n = pread(fd, b, (rem > 4096) ? 4096 : rem, (off_t)(skip + off));
		long w;
		if (n <= 0)
			break;
		w = 0;
		while (w < n) {
			long m = pwrite(fd, b + w, n - w, (off_t)(off + w));
			if (m <= 0)
				break;
			w += m;
		}
		off += n;
		rem -= n;
	}
	ftruncate(fd, 65536);
	close(fd);
}

/* webd 启动日志落盘：子进程 close_fds_ge(3) 后 fd1/2 已关，webd_main 里的
 * perror/启动输出会全部丢失 → 88 侧启动失败（socket/bind/listen 错误）在
 * 真机上无任何日志。这里把子进程 fd1/fd2 重定向到 /data/lx06/logs/webd.log
 * （append，>64K 原地轮转成 .1，fix15 硬上限）。同时写一行带 pid+时间的
 * 启动 banner，看门狗重拉时多行 banner 可数出重拉次数。LX06_WEBD_LOG 定义。
 * fix15：conf log_webd=0 → fd1/2 丢 /dev/null（日志开关=关）。 */
static void lx06_webd_log_redirect(void)
{
	char path[64];
	int lf;

	mkdir("/data/lx06", 0755);
	mkdir("/data/lx06/logs", 0755);
	if (!lx06_log_conf_on("log_webd")) {
		/* 关：当前文件截回 0（保持 inode，下次开启直接续写） */
		lx06_log_trunc_keep64k("/data/lx06/logs/webd.log");
		lf = open("/dev/null", O_WRONLY);
	} else {
		lx06_log_trunc_keep64k("/data/lx06/logs/webd.log");
		lf = open("/data/lx06/logs/webd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (lf < 0)
		return;   /* 写不进去也不影响 webd 主功能 */
	dup2(lf, 1);
	dup2(lf, 2);
	if (lf > 2)
		close(lf);
	if (!lx06_log_conf_on("log_webd"))
		return;
	snprintf(path, sizeof(path),
		 "webd(HTTP 88) 启动 pid=%d time=%lu\n", (int)getpid(),
		 (unsigned long)time(NULL));
	write(STDOUT_FILENO, path, strlen(path));
}

static volatile int lx06_http_pid = 0;

/* 关闭所有 >= minfd 的继承 fd（上限 open_max，兜底 4096）。
 * 用途：若本进程是从 webd 的 run API 的 sh 里拉起（调试场景），会继承旧
 * webd 的 88 listen fd，不关掉就绑不了 88。procd 正式拉起时 fd 干净，无副作用。 */
static void lx06_close_fds_ge(int minfd)
{
	int hi, i;
	hi = (int)sysconf(_SC_OPEN_MAX);
	if (hi <= 0 || hi > 4096)
		hi = 4096;
	for (i = minfd; i < hi; i++)
		close(i);
}

/* svr_main 最早期调用：清掉继承来的 fd（父进程侧）。 */
void lx06_unified_close_inherited(void)
{
	lx06_close_fds_ge(3);
}

#define LX06_UNIFIED_LOCK "/var/run/lx06-unified.lock"
static int lx06_instance_lock_fd = -1;

/* 单实例护栏（2026-10-18 真机定位）：portwatch 自愈 restart 与 S50/手工
 * start 并发时，老 procd 会留下两个主进程，各占 88/89 之一，其中不被
 * procd 跟踪的那个成为孤儿（重启/自愈都清不掉）。锁文件 flock 互斥：
 * 后到者等 4s（覆盖 stop→start 交接窗口）仍拿不到即退出，procd respawn
 * 重试，旧实例 stop 放锁后收敛为单实例。
 * fd 由主进程持有到退出；webd 子进程 fork 后关闭它（lx06_close_fds_ge），
 * 主进程被 SIGKILL 也不会遗留锁。打开失败不阻塞启动（退回旧行为）。
 * 返回 1=持有，0=他人在持（调用者应退出）。 */
int lx06_unified_instance_acquire(void)
{
	int fd, i;

	fd = open(LX06_UNIFIED_LOCK, O_RDWR | O_CREAT, 0644);
	if (fd < 0)
		return 1;
	for (i = 0; i < 20; i++) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			char pb[16];
			int pn;
			lx06_instance_lock_fd = fd;
			pn = snprintf(pb, sizeof(pb), "%d", (int)getpid());
			(void)ftruncate(fd, 0);
			(void)lseek(fd, 0, SEEK_SET);
			(void)!write(fd, pb, (size_t)pn);
			return 1;
		}
		if (errno != EWOULDBLOCK && errno != EAGAIN)
			break;
		usleep(200 * 1000);
	}
	/* 拿不到锁：日志里带上持锁者 pid（/proc/locks 的 pid 是 flock 的
	   真实持有者——fd 继承链可见；锁文件文本 pid 可能已陈旧） */
	{
		char holder[256] = "", ino_s[32] = "", lline[256];
		struct stat lsb;
		FILE *lf = fopen("/proc/locks", "r");
		if (stat(LX06_UNIFIED_LOCK, &lsb) == 0)
			snprintf(ino_s, sizeof(ino_s), ":%lu ", (unsigned long)lsb.st_ino);
		if (lf && ino_s[0]) {
			while (fgets(lline, sizeof(lline), lf)) {
				char *wp = strstr(lline, "WRITE");
				/* 格式: "1: FLOCK  ADVISORY  WRITE <pid> <maj:min>:<ino> ..." */
				if (wp && strstr(wp, ino_s)) {
					char *pp = wp + 5; /* "WRITE" 后第一 token = pid */
					while (*pp == ' ') pp++;
					{
						char *e = pp;
						while (*e && *e != ' ' && *e != '\n') e++;
						if (e > pp) {
							*e = 0;
							strncpy(holder, pp, sizeof(holder) - 1);
						}
					}
					break;
				}
			}
			fclose(lf);
		}
		dropbear_log(LOG_INFO, "lx06: 统一实例锁被持有(/proc/locks=%s)，本实例退出",
		             holder[0] ? holder : "?");
		/* 全量转储 /proc/locks 供事后分析（格式/字段变化时看原始行） */
		{
			FILE *lf2 = fopen("/proc/locks", "r");
			FILE *df = fopen("/data/lx06/logs/locks-dump.txt", "a");
			if (lf2 && df) {
				fprintf(df, "---- pid=%d %s\n", (int)getpid(),
				        "lock-held");
				while (fgets(lline, sizeof(lline), lf2))
					fputs(lline, df);
				fclose(df);
			}
			if (lf2)
				fclose(lf2);
		}
	}
	close(fd);
	return 0;
}

/* 会话子进程 fork 后调用（2026-10-18 真机根因）：dropbear 每个 SSH
 * 连接 fork 的子进程会继承主进程的实例锁 fd，主进程退出后孤儿子进程
 * 仍持 flock → 新实例拿不到锁起不来（锁被持有时间 = 该 SSH 会话寿命）。
 * webd 子进程走 close_fds_ge(3) 已覆盖；会话子进程只关 listen socket，
 * 须显式关锁 fd。 */
void lx06_unified_close_lock_fd(void)
{
	if (lx06_instance_lock_fd >= 0) {
		close(lx06_instance_lock_fd);
		lx06_instance_lock_fd = -1;
	}
}

/* 拿锁后清理陈旧 88：主进程被 SIGKILL 时其 webd 子进程（ppid 已归 1）
 * 可能仍占着 88；此时新主进程尚未 fork 自己的子进程，88 属主只可能是
 * 残留子进程 → TERM+1s+KILL。持锁保证没有其他活的主进程，不会误杀。 */
void lx06_unified_kill_stale_webd(void)
{
	char line[256], want[32], fddir[64], fpath[96], target[64];
	int ino = 0, holder = 0, i;
	FILE *f;
	DIR *pdir;
	struct dirent *de;

	want[0] = 0;
	f = fopen("/proc/net/tcp", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			/* 不用 sscanf（真机定位：该 glibc 下对 /proc/net/tcp 行解析
			   错位，st/ino 读到垃圾值）。按空白分列：
			   [0]sl [1]local [2]rem [3]st [4]tx:rx [5]tr:tm
			   [6]retrnsmt [7]uid [8]timeout [9]inode */
			char *cols[12], *save = NULL;
			int nc = 0, ci;
			char *tok = strtok_r(line, " 	\r\n", &save);
			while (tok && nc < 12) {
				cols[nc++] = tok;
				tok = strtok_r(NULL, " 	\r\n", &save);
			}
			if (nc >= 10) {
				char *cp = strrchr(cols[1], ':');
				if (cp && cols[3][0] == '0' && cols[3][1] == 'A' &&
				    (int)strtol(cp + 1, NULL, 16) == LX06_WEBD_PORT) {
					ino = (int)strtol(cols[9], NULL, 10);
					break;
				}
			}
		}
		fclose(f);
	}
	if (ino <= 0)
		return;
	snprintf(want, sizeof(want), "socket:[%d]", ino);
	pdir = opendir("/proc");
	if (!pdir)
		return;
	while ((de = readdir(pdir)) != NULL) {
		DIR *fdir;
		struct dirent *fde;
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(fddir, sizeof(fddir), "/proc/%s/fd", de->d_name);
		fdir = opendir(fddir);
		if (!fdir)
			continue;
		while ((fde = readdir(fdir)) != NULL) {
			ssize_t n;
			snprintf(fpath, sizeof(fpath), "/proc/%s/fd/%s",
			         de->d_name, fde->d_name);
			n = readlink(fpath, target, sizeof(target) - 1);
			if (n > 0)
				target[n] = 0;
			if (n > 0 && strcmp(target, want) == 0)
				holder = atoi(de->d_name);
		}
		closedir(fdir);
	}
	closedir(pdir);
	if (holder <= 0 || holder == (int)getpid())
		return;
	dropbear_log(LOG_INFO, "lx06: 88 被残留进程 %d 占用，先行清理", holder);
	kill(holder, SIGTERM);
	for (i = 0; i < 10; i++) {
		if (kill(holder, 0) != 0)
			break;
		usleep(100 * 1000);
	}
	kill(holder, SIGKILL);
}

/* fork 一个 webd 子进程跑 HTTP 88。主进程已 setsid+SIG_IGN SIGCHLD，
 * 子进程关掉全部继承 fd（含 dropbear 的 22 listen fd）后跑 webd_main。
 * webd 挂掉不自动重拉（procd 重启整个 lx06unified 即可恢复，SSH 不受影响）。
 * 返回子 pid（0 表失败）。 */
int lx06_unified_http_start(void)
{
	pid_t p;
	if (lx06_http_pid > 0) return lx06_http_pid;   /* 已拉起 */
	p = fork();
	if (p < 0) {
		dropbear_log(LOG_INFO, "lx06: fork webd 失败: %s", strerror(errno));
		return 0;
	}
	if (p == 0) {
		/* 子进程：丢继承 fd，独占 88 */
		lx06_close_fds_ge(3);
		lx06_webd_log_redirect();   /* fd1/2 → webd.log（88 侧启动/崩溃日志） */
		webd_main();
		_exit(0);   /* webd_main 不返回 */
	}
	/* 父进程 */
	lx06_http_pid = (int)p;
	dropbear_log(LOG_INFO, "lx06: webd(HTTP 88) 已拉起 pid=%d", (int)p);
	return (int)p;
}

int lx06_unified_http_pid(void)
{
	return lx06_http_pid;
}

/* procd stop / 主进程退出时带走 webd 子进程（同进程组，SIGTERM 后
 * 宽限 1s 强杀）。svr_dropbear_exit 调用，LX06_UNIFIED 下编译。
 *
 * 坑（2026-10-11 真机定位）：dropbear 每个 SSH 会话是主进程 fork 的子进程，
 * 会话退出也走 svr_dropbear_exit → 会带着 fork 后继承来的 lx06_http_pid
 * 把 webd SIGKILL 掉（kill 跨进程有效，都是 root）。表现为「每次 SSH 会话
 * 一断开 webd 就死、看门狗 5s 后重拉」。修法：只有 webd 的真实父进程
 * （/proc/<pid>/stat 的 ppid == self）才有权杀；会话子进程退出时 webd 的
 * ppid 是主进程 ≠ self，直接放行。webd 已死/ pid 复用 → open 失败或 ppid
 * 不匹配，同样不碰。 */
void lx06_unified_http_stop(void)
{
	if (lx06_http_pid <= 0)
		return;
	{
		char statpath[32], buf[128], *cp;
		int sf, ppid = -1;

		snprintf(statpath, sizeof(statpath), "/proc/%d/stat",
			 lx06_http_pid);
		sf = open(statpath, O_RDONLY);
		if (sf >= 0) {
			ssize_t n = read(sf, buf, sizeof(buf) - 1);
			close(sf);
			if (n > 0) {
				buf[n] = 0;
				cp = strrchr(buf, ')');
				if (cp) {
					/* /proc/pid/stat: "pid (comm) STATE ppid ..."。
					   ) 后是空格+state(1字符)+空格，atoi(cp+1) 撞上 state
					   永远得 0（2026-10-18 真机定位：webd 子进程因此从不被
					   stop 带走，88 遗留旧实例）。手工跳过 state 字段。 */
					const char *s = cp + 1;
					while (*s == ' ') s++;
					s++;
					while (*s == ' ') s++;
					ppid = (int)strtol(s, NULL, 10);
				}
			}
		}
		if (ppid != getpid()) {
			lx06_http_pid = 0;
			return;
		}
	}
	kill(lx06_http_pid, SIGTERM);
	usleep(1000000);
	kill(lx06_http_pid, SIGKILL);
	lx06_http_pid = 0;
}

/* ---- webd 看门狗：webd 子进程挂掉后自动重拉（主进程不退出）。
 *
 * 探活：connect(127.0.0.1:88)。监听在 = 活着；ECONNREFUSED = 死了。
 * （主进程 SIG_IGN(SIGCHLD)，子进程退出由内核自动回收、无信号可等，
 *   端口探活是唯一可靠判据。）
 * 重拉前只 kill 记录 pid 当 /proc/<pid> 仍存在时（= 子进程卡死未回收，
 * 确是它）；已回收则不 kill —— 防止 pid 复用误杀无辜进程。
 * 失败/重拉后 5s 退避，避免 webd 持续 crash 时 connect 风暴。
 * 由 svr_main 主循环每次 select 返回后调用。 */
void lx06_unified_http_watchdog(void)
{
	static time_t last_try = 0;
	time_t now;
	struct sockaddr_in sin;
	struct timeval tv;
	char procpath[32];
	int fd;

	if (lx06_http_pid <= 0) return;
	now = time(NULL);
	if (now - last_try < 5) return;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;
	tv.tv_sec = 0;
	tv.tv_usec = 200000;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(LX06_WEBD_PORT);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *) &sin, sizeof(sin)) == 0) {
		close(fd);                 /* 88 在听 = webd 活着 */
		return;
	}
	close(fd);
	if (errno != ECONNREFUSED) {
		last_try = now;           /* 非 ECONNREFUSED 也走退避，防 select 热路径风暴 */
		return;
	}

	/* 判死：子进程仍占着 pid（卡死/未回收）才 kill；已回收则直接重拉。
	 * B3 修复（fix14）：kill 后轮询等 88 端口真正释放（SIGKILL 到内核回收
	 * listen socket + 存量连接 TIME_WAIT 有滞后），确认端口空了才重拉，
	 * 避免「新 webd bind: Address already in use → 崩 → 看门狗再拉」死循环。
	 * 端口仍被占（旧进程死得慢）→ 本轮不重拉，交给 5s 后下一轮看门狗。 */
	dropbear_log(LOG_INFO, "lx06: webd(HTTP 88) 看门狗检测到端口关闭，重拉");
	snprintf(procpath, sizeof(procpath), "/proc/%d", lx06_http_pid);
	if (access(procpath, F_OK) == 0) {
		kill(lx06_http_pid, SIGKILL);
	}
	lx06_http_pid = 0;
	last_try = now;
	/* 等端口释放：最多 20×250ms=5s。connect 成功=还有东西占着，继续等。 */
	{
		int ok = 0, i;
		for (i = 0; i < 20; i++) {
			int f2 = socket(AF_INET, SOCK_STREAM, 0);
			if (f2 >= 0) {
				struct timeval tv2;
				tv2.tv_sec = 0;
				tv2.tv_usec = 150000;
				setsockopt(f2, SOL_SOCKET, SO_SNDTIMEO, &tv2, sizeof(tv2));
				if (connect(f2, (struct sockaddr *) &sin, sizeof(sin)) == 0) {
					close(f2);      /* 仍有人监听 → 没释放 */
					usleep(250000);
					continue;
				}
				close(f2);
				ok = 1;             /* ECONNREFUSED = 端口空了 */
				break;
			}
			usleep(250000);
		}
		if (!ok) {
			dropbear_log(LOG_INFO, "lx06: 88 端口未释放, 本轮不重拉（5s 后重试）");
			return;
		}
	}
	lx06_unified_http_start();
}
