/* Speaker inbox API 6, PROVISIONAL(P-20261111-0532-B/P-20261111-0749-B/P-20261111-1523-B/P-20261111-1557-B/P-20261111-1614-B/P-20261111-1903-B/P-20261113-0345-B).
 * Typed input AFTER authenticated Wire/JSON parsing. No settings parser or
 * phone Ledger here. Caller serializes calls and provides original association.
 * Unix ms only with trusted time; operation deadlines elsewhere use monotonic.
 */
#ifndef LX06_RESULT_INBOX_H
#define LX06_RESULT_INBOX_H
#include "inbox_store.h"
#include <stdint.h>
#define BR_INBOX_ROWS 64u
#define BR_INBOX_PENDING 16u
enum br_result_state { BR_QUEUED=1,BR_PLAYING,BR_DONE,BR_FAILED,BR_INTERRUPTED,BR_EXPIRED,BR_SILENT,BR_SUPPRESSED };
enum br_received { BR_INVALID=-1,BR_IO=-2,BR_ACCEPTED=1,BR_DUPLICATE,BR_DEFER_CLOCK,BR_DEFER_LEDGER,BR_ACCEPTED_SILENT,BR_ALREADY_TERMINAL };
enum br_speech_policy { BR_SPEECH_ORIGINAL=0,BR_SPEECH_SCHEDULED=1,BR_SPEECH_TIMEOUT=2 };
typedef struct br_result {
    char task_id[129],command_id[129],app_version[129],hook_version[129],code[40],text[2049];
    uint64_t epoch;
    /* Local API2 only, PROVISIONAL(P-20261111-1523-B). Never on Wire.
     * origin=0 Ledger (default); origin=1 synchronous local receipt;
     * API4 origin=2 local timeout NOTICE, never a terminal receipt.
     * Local rows have EMPTY remote task/app/hook versions, no placeholders. */
    unsigned origin;
    struct { char script[129],revision[65],task[33];uint64_t serial; } local;
} br_result;
/* Ledger-only typed-row validation for transport adapters. Local origin is
 * deliberately INVALID here; it never impersonates a phone Ledger result. */
int br_result_valid(const br_result *);
typedef struct br_result_row { br_result result;uint64_t received_ms,expires_ms;unsigned state,audible,speech_policy; } br_result_row;
typedef struct br_inbox {
    br_store *store;
    int (*associate)(void *,const br_result *);
    void *context;
    int (*sync_associate)(void *,const br_result *);
    unsigned schema;
    int blocked; /* failed restore must never become an empty writable queue */
    unsigned count;
    br_result_row rows[BR_INBOX_ROWS];
} br_inbox;
int br_inbox_open(br_inbox *,br_store *,int (*associate)(void *,const br_result *),void *context);
int br_inbox_open_sync(br_inbox *,br_store *,int (*associate)(void *,const br_result *),
                       int (*sync_associate)(void *,const br_result *),void *context);
/* Local typed input only; same queue/store/TTL and playback APIs. */
int br_inbox_sync_receive(br_inbox *,const br_result *,uint64_t now_ms,int trusted,int audible,unsigned ttl_s);
/* API3: explicit scheduled terminal speech only. Old APIs select ORIGINAL.
 * First durable row freezes policy; identical duplicates never upgrade it.
 * Timeout policy is reserved for br_inbox_timeout, not terminal input. */
int br_inbox_sync_receive_policy(br_inbox *,const br_result *,uint64_t now_ms,int trusted,int audible,unsigned ttl_s,enum br_speech_policy);
/* API4: original empty origin1 identity -> separate origin2 notice. The caller
 * proves deadline expiry; owner association must explicitly allow origin2.
 * No terminal proof; sync_receipt never counts this notice. Once durable,
 * original late terminal results are stored SUPPRESSED with original text.
 * BR_ALREADY_TERMINAL means a terminal already exists; no notice was stored. */
int br_inbox_timeout(br_inbox *,const br_result *identity,uint64_t now_ms,int trusted,int audible,unsigned ttl_s);
/* identity has empty code/text. 1 original durable receipt + output mode,
 * 0 absent, BR_INVALID mismatched association, BR_IO blocked. No new enqueue. */
int br_inbox_sync_receipt(br_inbox *,const br_result *identity,int audible);
/* PROVISIONAL(P-20261111-1653-B): same original receipt check, borrowed result
 * until next inbox mutation. Failure sets out=NULL. No new admission/queue. */
int br_inbox_sync_result(br_inbox *,const br_result *identity,int audible,const br_result **out);
/* PROVISIONAL(P-20261111-2108-B): original origin0 identity (empty code/text),
 * exact epoch/command/task/app/hook and CURRENT association. Read-only borrowed
 * result, out=NULL on absence/error. No receipt creation, TTL or playback. */
int br_inbox_ledger_result(br_inbox *,const br_result *identity,const br_result **out);
/* Local API6, PROVISIONAL(P-20261113-0345-B): pointer-free UI observation of
 * the UNIQUE original inbox, not a second result queue or receipt API.
 * identity is an original origin0/1/2 result with EMPTY code/text. Exact
 * epoch/command/task/versions or local script/revision/task/serial required;
 * origin2 selects the timeout NOTICE, independently of its late terminal.
 * The original serial owner calls BETWEEN inbox/player calls. Association
 * callbacks must remain pure, bounded proofs: no IO, mutation or reentry into
 * the inbox/player (including this reader). Objects/identity outlive the call;
 * out is disjoint from them. No clock, allocation, polling, expiry mutation,
 * store read/write, reservation, playback or callback ownership is added.
 * 1 copies, 0 absent, BR_INVALID invalid/revoked, BR_IO blocked/poisoned.
 * All non-1 returns leave out unchanged. Current association is required even
 * when absent. This RAM view does NOT re-read disk or prove durable ownership.
 *
 * state is original BR_QUEUED..BR_SUPPRESSED SPEECH state: BR_DONE means sound
 * finished, NOT business success. terminal=0 for origin2 (even if BR_DONE),
 * 1 for a stored terminal receipt; no UNKNOWN/reap/retirement authority.
 * Original code/text only; no fallback speech or synthesized success. Text is
 * an untrusted plain display prefix of <=512 UTF-8 bytes, never TTS/commands.
 * text_bytes is length, truncated marks remaining text; all padding/tails zero.
 * received_ms/expires_ms are original trusted-wall timestamps, not a current
 * clock/TTL decision. The copy is historical immediately; consumers match ALL
 * identity fields before applying it and discard it after any owner mutation.
 * No logging/persistence of the view or public export of private result text. */
typedef struct br_result_progress {
    uint64_t epoch,serial,received_ms,expires_ms;
    uint16_t text_bytes;
    uint8_t origin,state,audible,speech_policy,terminal,truncated;
    char command_id[129],task_id[129],app_version[129],hook_version[129];
    char script[129],revision[65],task[33],code[40],text[513];
} br_result_progress;
int br_inbox_progress_read(br_inbox *,const br_result *identity,br_result_progress *out);
/* Empty origin1 identity, CURRENT association even when no row remains.
 * 1 authorized, BR_INVALID rejected, BR_IO blocked. No receipt proof/queue IO.
 * For a separately durable journal witness, never permission to re-admit. */
int br_inbox_sync_authorized(br_inbox *,const br_result *identity);
/* live=1 TASK45 must match connection epoch; live=0 task.list keeps old epoch.
 * No receipt ACK or new frame. save failure never acknowledges acceptance. */
int br_inbox_receive(br_inbox *,const br_result *,int live,uint64_t connection_epoch,
                     uint64_t now_ms,int trusted,int audible,unsigned ttl_s);
int br_inbox_receive_policy(br_inbox *,const br_result *,int live,uint64_t connection_epoch,
                            uint64_t now_ms,int trusted,int audible,unsigned ttl_s,enum br_speech_policy);
/* 1 reserved (intent durable),0 deferred,-1 invalid/-2 store error. K5 acquire
 * still mandatory. Busy K5 retains intent for retry; reboot never replays it. */
int br_inbox_take(br_inbox *,uint64_t now_ms,int trusted,int idle,br_result *out);
/* PROVISIONAL(P-20261111-1541-B): revalidate a durable PLAYING reservation
 * before a delayed K5 acquire. Read-only, exact original row required. */
enum br_play_check { BR_PLAY_WAIT=0,BR_PLAY_READY=1,BR_PLAY_EXPIRED,BR_PLAY_REVOKED };
int br_inbox_playback_check(br_inbox *,const br_result *,uint64_t now_ms,int trusted);
/* Exact original PLAYING row + original association required. 1 supplies
 * borrowed immutable text (possibly empty), negative invalid/IO leaves out
 * NULL. Never changes result/code. Consume before the next inbox mutation. */
int br_inbox_speech(br_inbox *,const br_result *,const char **out);
int br_inbox_finish(br_inbox *,const br_result *,enum br_result_state status);
#endif
