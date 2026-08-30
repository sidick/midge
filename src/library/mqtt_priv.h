#ifndef MIDGE_MQTT_PRIV_H
#define MIDGE_MQTT_PRIV_H

/* mqtt.library private (implementation-only) header. Not installed for
 * callers - see src/library/include/libraries/mqtt.h for the public API.
 *
 * Architecture (see CLAUDE.md / the Phase 2 slice 2 task notes): each APTR
 * client handle is one dedicated AmigaOS subprocess owning the bsdsocket
 * open, the socket, and an unchanged src/core mqtt_client instance.
 * Caller-side library functions (src/library/mqtt_funcs.c) talk to it via
 * two MsgPorts:
 *
 *   ch_MsgPort - CALLER-task-owned. Delivery: the child PutMsg()s one
 *                struct MqttMessage per PUBLISH here; MQTT_GetMessage()
 *                GetMsg()s it.
 *   ch_CmdPort - CHILD-task-owned. Commands: the caller PutMsg()s an
 *                MqttCmdMsg here and Wait()s for the reply, exactly like
 *                amiauth's src/amiga/guiport.c - the child holds pointers
 *                into the caller's stack frame for the duration of the
 *                exchange, so the caller must never abandon the wait.
 */

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <dos/dos.h>

#include "libraries/mqtt.h"
#include "mqtt_client.h"

/* --- Commands, caller task -> child connection subprocess ---------------- */
#define MQTTCMD_CONNECT    1
#define MQTTCMD_PUBLISH    2
#define MQTTCMD_SUBSCRIBE  3
#define MQTTCMD_DISCONNECT 4
#define MQTTCMD_QUIT       5 /* internal, sent only by MQTT_DeleteClient() */

/* One command exchange. Lives on the CALLER's C stack for the duration of
 * the PutMsg()/Wait()/GetMsg() round trip in mqtt_funcs.c's do_command() -
 * the child only ever touches it between receiving it and ReplyMsg()ing it
 * back, so it's safe there despite not being heap-allocated. */
typedef struct {
    struct Message cm_Msg;
    UWORD    cm_Cmd;
    LONG     cm_Result;   /* filled by the child: 0 or a negative
                              mqtt_err/mqtt_client_err/MQTTERR_* code */
    mqtt_str cm_Topic;    /* PUBLISH topic / SUBSCRIBE filter - points into
                              the caller's own argument, valid for the
                              exchange's duration only */
    const UBYTE *cm_Payload;
    ULONG    cm_PayloadLen;
    LONG     cm_Retain;
    UBYTE    cm_Qos;
} MqttCmdMsg;

/* One-shot startup handshake: the parent PutMsg()s this to the freshly
 * created child's own &proc->pr_MsgPort (every AmigaOS process has one
 * built in); the child WaitPort()s/GetMsg()s it there, creates its command
 * port, fills in st_CmdPort, and ReplyMsg()s it back. Mirrors guiport.c's
 * request/reply shape. */
typedef struct {
    struct Message st_Msg;
    struct MqttClientHandle *st_Handle; /* in: set by the parent before PutMsg */
    struct MsgPort *st_CmdPort;         /* out: set by the child before
                                            ReplyMsg(); NULL means the child
                                            could not start (no memory for
                                            its own command port) */
} MqttStartupMsg;

/* The APTR handle MQTT_CreateClient() returns. AllocVec'd (MEMF_PUBLIC |
 * MEMF_CLEAR) and owned by the CALLER's task - every MQTT_* call on it
 * (including MQTT_GetMessage/MQTT_FreeMessage/MQTT_DeleteClient) must come
 * from that same task, per the public header's threading-model note.
 *
 * ch_Host and the ch_Opts string fields are deep copies made at
 * MQTT_CreateClient() time (one AllocVec each) - the caller may free or
 * reuse whatever it originally passed in immediately after the call
 * returns. They live for the handle's lifetime; the child subprocess only
 * ever reads them (at MQTTCMD_CONNECT time), never writes them, so sharing
 * them across the caller/child tasks needs no locking. */
typedef struct MqttClientHandle {
    struct MsgPort *ch_MsgPort; /* delivery port; CALLER-owned */
    struct MsgPort *ch_CmdPort; /* command port; CHILD-owned */
    struct Process *ch_Child;
    int ch_Connected;           /* MQTT_Connect() succeeded and
                                    MQTT_Disconnect() hasn't run since */

    STRPTR ch_Host;
    UWORD  ch_Port;
    struct MqttConnectOpts ch_Opts; /* mco_ClientID/Username/Password point
                                        at this handle's own AllocVec'd
                                        copies below, or NULL */
    STRPTR ch_ClientID;
    STRPTR ch_Username;
    STRPTR ch_Password;
    STRPTR ch_CAFile;

    /* --- QoS 1 publish / SUBACK-wait scratch state -------------------------
     * Written by deliver_cb() (called from inside the CHILD subprocess's own
     * mqtt_client_process() pump) whenever it sees an acknowledgement-class
     * packet; read back by that same child subprocess's command handlers
     * (MQTTCMD_PUBLISH/MQTTCMD_SUBSCRIBE in mqtt_funcs.c's child_run()) right
     * after each process() call, in the same task. Never touched by the
     * caller's task - at most one command is ever in flight per handle
     * (do_command() is a synchronous round trip), so there is no concurrent
     * access to guard against. Cleared by the child before each wait loop
     * that uses them. */
    int    ch_AckSeen;  /* set by deliver_cb() when it records a fresh ack */
    UBYTE  ch_AckType;  /* MQTT_PUBACK or MQTT_SUBACK (mqtt_packet.h) */
    UWORD  ch_AckId;    /* the ack's packet id */
    UBYTE  ch_AckCode;  /* SUBACK: pkt->u.suback.codes[0] (this library only
                            ever subscribes one filter per SUBSCRIBE, so the
                            first grant code is the whole answer); unused for
                            PUBACK */
} MqttClientHandle;

/* --- mco_AutoReconnect (libraries/mqtt.h): remembered-subscriptions list --
 * The child subprocess (mqtt_funcs.c's child_run()) keeps a singly linked
 * list of every filter successfully MQTT_Subscribe()'d since the last
 * MQTT_Connect(), so it can reissue them all after an auto-reconnect. This
 * is CHILD-task-local state (a child_run() local variable, not a field of
 * MqttClientHandle above - it is never touched by the caller's task, so it
 * doesn't need to live in the caller-visible handle). See
 * mqtt_funcs.c's SubNode/sub_list_add()/sub_list_free()/
 * child_reconnect_loop(). */

#endif /* MIDGE_MQTT_PRIV_H */
