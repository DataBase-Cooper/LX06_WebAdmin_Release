/* webd_standalone.c —— 统一二进制入口。
 * webd.c 定义 webd_main()；本文件提供 main() 直接调它（单进程同时服务
 * HTTP 88；SSH 22 由 dropbear 提供，二者共用 /data/lx06-ssh.conf 密码源）。
 * 独立成文件是为了同一份 webd.c 既能编 standalone（带 main）又能嵌
 * 入其他宿主（只需链接 webd_integrated.o 并自己调 webd_main）。 */
int webd_main(void);
int webd_extension_main(int argc, char **argv);
int webd_check_extension(const char *path);
#include <string.h>
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--check-extension"))
        return webd_check_extension(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "--extension-run"))
        return webd_extension_main(argc, argv);
    return webd_main();
}
