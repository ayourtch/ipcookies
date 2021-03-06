/********************************************************************

This implements the conceptual IP cookie algorithm in a verbatim
and naive way to ensure the simplicity and provide a reference
implementation.

The aim of this algorithm is to eventually mitigate the source-spoofing
amplification attacks related to protocols which trust the source
address of the datagram.

It consists of two parts: a cookie daemon and the shim.

Cookie daemon maintains two structures in memory shared with the shim's
instances:

ipcookie_state_t ipcookie_state;
ipcookie_cache_t ipcookie_cache;

The first one maintains the cookie-related state for the host
for the stateless "server" portion of the cookies:
(also the below include file defines how the cookie is calculated)

********************************************************************/

#include "ipcookies_stateless.h"

/********************************************************************

The second data structure is the cookie state maintained for the peers
requesting the cookie use, as well as a scratchpad to keep track
the explicit non-usage of the cookies for specific peers as both
client and server in order to perform the fallback (the remote side
supports the cookies, but the required messages do not pass.

The entries in this table are created when the local side is the initiator
of the communication.

They are being used to fill in the cookie value in outgoing packets,
and also to allow the verification of the control messages.

********************************************************************/

typedef struct ipcookie_entry {
  struct in6_addr peer;    /* Which peer this is for */
  uint16_t mtime_lo16;     /* Low bits of timestamp when this entry was
                              last modified (aka when we saw the previous SET-COOKIE) */
  uint8_t mtime_hi8;       /* high 8 bits of timestamp  */
  uint8_t  flags_and_lifetime_log2;  /* Upper four bits are flags, lower four bits are log2 lifetime */
  ipcookie_t ipcookie;     /* The ipcookie itself */
} ipcookie_entry_t;

#define IPCOOKIE_LIFETIME_LOG2_INFINITE 0xF


/* #define IPCOOKIE_ENTRY_FLAG_DISABLE_COOKIES please_use_functions_below_to_check_and_set */

void ipcookie_entry_set_disable_cookies(ipcookie_entry_t *ce);
void ipcookie_entry_clear_disable_cookies(ipcookie_entry_t *ce);
int ipcookie_entry_isset_disable_cookies(ipcookie_entry_t *ce);

/* #define IPCOOKIE_ENTRY_FLAG_EXPECTING_SETCOOKIE please_use_functions_below_to_check_and_set */

void ipcookie_entry_set_expecting_setcookie(ipcookie_entry_t *ce);
void ipcookie_entry_clear_expecting_setcookie(ipcookie_entry_t *ce);
int ipcookie_entry_isset_expecting_setcookie(ipcookie_entry_t *ce);

#include "ipcookies_cache.h"

typedef struct ipcookie_full_state {
  ipcookie_state_t state;
  ipcookie_cache_t cache;
} ipcookie_full_state_t;



/********************************************************************

There are two ICMP messages in the protocol: SET-COOKIE and
SETCOOKIE-NOT-EXPECTED.

Cookie daemon's job is relatively simple: listen to the received
ICMP messages, verify them against the existing cookie,
and if this verification process passes, update the cookie
values into the table.

The first one is the message to set/correct the cookie, sent by a responder
peer.

If we receive SET-COOKIE and the cookie entry for the peer is not present,
we copy the sender's cookie into an SETCOOKIE-NOT-EXPECTED message and
reply with that message back to the sender.

If we receive SET-COOKIE and the cookie entry for the peer exists,
we verify that echoed cookie in this message matches the entry,
and then use the suggested cookie to update the table.
We also update the lifetime_log2 from the received packet.
This will allow us to (somewhat) detect the blackholes which
can arise later if the network topology changes.

If we receive SETCOOKIE-NOT-EXPECTED, we verify its cookie
using the stateless cookie creation algorithm for that peer, if there
is a match, this means it is a valid reaction to the SET-COOKIE
which we sent, this means the original data packet triggering SET-COOKIE
was spoofed. This is a loggable event.

If we receive SETCOOKIE-NOT-EXPECTED and the stateless cookie verification
fails, this means this notification has been spoofed and it MUST be
ignored, possibly with rate-limited logging.

********************************************************************/


/********************************************************************

The shim's job is two fold:
  * on the receive path, if the received packets contain the cookie,
    it triggers the verification/update signaling of the cookie if needed.
  * on the send path, it creates and maintains the state of per-peer
    cookies, and also deals with fallback in case the cookies or
    the ICMP signaling are blocked on the path.

To help with the cookie updates, the following variables are defined,
with the values adjustable by the local host.

IPCOOKIE_T_RECOVER:
           interval (in seconds) to await for the SET-COOKIE message
           in reply to sent-out packet with cookie. After this period
           ends, the implementation falls back to cookie-less sending.

IPCOOKIE_FALLBACK_LT2:
           a log2 value of the time of cookie-less operation in case
           we detect the problem with signaling. In this implementation
           this is a host-wide constant, but can be optimized by the hosts
           since it is locally significant.

IPCOOKIE_TRY_LT2:
           a log2 value of the time to try the cookies when the fallback
           period has expired and we are retrying to use the cookie again.
********************************************************************/

#define IPCOOKIE_T_RECOVER 3

/* 2^8 = 256 seconds */
#define IPCOOKIE_FALLBACK_LT2 8

/* 2^3 = 8 seconds */
#define IPCOOKIE_TRY_LT2 3


/********************************************************************

The below text discusses the operation of the shim on the receive
and send paths.

On the receive path:

It needs to verify the incoming packets
which contain the cookie destination option against the stateless
server CURRENT cookie, and if that fails, attempt to verify against
server PREVIOUS cookie.

If the verification against CURRENT cookie fails,
the shim needs to send the SET-COOKIE message containing the
value of the calculated CURRENT cookie, and a copy of the received
cookie in the echo-cookie field. The packet needs to also contain
a copy of the halflife_log2 field to inform the remote side on when
to expect an update of the cookie. No new state is created.

If the received cookie verifies against either the CURRENT or
PREVIOUS calculated cookies, then the received datagram can be passed
further to the host stack.

On the send path:

It needs to look up the cookie cache if an entry for a given peer
exists. If it exists, we act according to following:

ENTRY EXISTS {
We need to check the current timestamp to be within three cases:
case 0 : below (mtime_ts+2^lifetime_log2)
case 1 : between mtime_ts+2^lifetime_log2 and mtime_ts+2^lifetime_log2+IPCOOKIE_T_RECOVER
case 2 : above mtime_ts+2^lifetime_log2 + IPCOOKIE_T_RECOVER

********************************************************************/

typedef enum {
  IPCOOKIE_TS_STILL_VALID = 0,
  IPCOOKIE_TS_RENEW_TIME,
  IPCOOKIE_TS_PAST_RENEW_TIME
} ipcookie_ts_check_t;

ipcookie_ts_check_t check_ipcookie_entry_timestamp(ipcookie_entry_t *ce);

/********************************************************************

the following two groups are describing behavior depending on whether the DISABLE_COOKIES flag
is set or not.

DISABLE_COOKIES is set:
case 0: do nothing.
case 1: same as the case 2:
case 2: fallback wait-out period has expired. Clear DISABLE_COOKIES, update
the mtime_ts with the current timestamp, and set the lifetime_log2 to
a policy-defined value of "COOKIE_TRY_LT2".

DISABLE_COOKIES is cleared:
case 0: do nothing.
case 1: is IPCOOKIE_EXPECTING_SETCOOKIE flag set ?
  * yes: do nothing
  * no: this is the first outbound packet during the rollover period.
    set the IPCOOKIE_EXPECTING_SETCOOKIE.
    Also, we need to ensure we always wait for the entirety of IPCOOKIE_T_RECOVER
    period to receive SET-COOKIE, once we sent the packet out.
    If we do not do this, if we were in the very end of the rollover period,
    we might spuriously enter the recovery mode. Therefore the timestamp needs
    to be set to (time_now-2^lifetime_log) as well.
case 2: is IPCOOKIE_EXPECTING_SETCOOKIE flag set ?
  * yes: that means we did sent traffic earlier but did not hear anything back.
    Therefore we can assume that the path has changed and the cookies or
    ICMP signaling are blocked, so we need to set the DISABLE_COOKIES and
    update the mtime_ts to the current timestamp. We also need to set
    the lifetime_log2 to a policy-defined value which will determine
    the duration of the fallback period - "COOKIE_FALLBACK_LT2".
  * no: this means that we missed the time window due to not sending the traffic.
    We can not just set the IPCOOKIE_EXPECTING_SETCOOKIE, because the next egress
    packet would cause a fallback process to be triggered. So, together with setting
    the IPCOOKIE_EXPECTING_SETCOOKIE flag we need to ALSO "rewind"
    the timestamp by setting it to a value (time_now-2^lifetime_log) - so that we
    allow the protocol on the other side IPCOOKIE_T_RECOVER seconds to react and send the SETCOOKIE
    before the fallback is triggered.

NB: There might be other ways to optimize of the recovery, but since the recovery
algorithm is locally significant on the initiator, the above paragraph is not a hard
requirement for a node to implement verbatim.

}

If the entry does not exist:

ENTRY DOES NOT EXIST {
  we need to allocate a new entry for this peer, using FIFO or some other
  queue management algorithm to evict the old entries.

  The new entry has mtime_ts set to the current timestamp.

  The new entry gets DISABLE_COOKIES flag set or cleared depending on the local policy -
  whether the host wishes to use this mechanism with the conversation or not.

  If the DISABLE_COOKIES is cleared (cookies active): set the lifetime_log2 to zero, and set
  IPCOOKIE_EXPECTING_SETCOOKIE flag.

  If the DISABLE_COOKIES is set (cookies inactive): set the lifetime_log2 to 0xf ("infinity"),
  or to another value if the application would like to try the cookies at a later time in lifetime
  of the conversation. clear the IPCOOKIE_EXPECTING_SETCOOKIE flag.


}

The next thing we do is make a decision whether to add the cookie option
to the packet, or to send it as is. For this we will look at
the DISABLE_COOKIES flag. In this conceptual implementation we present this
as two separate lookups with the above, to modularize it. A more optimized
implementation is certainly possible, but is not a focus at this time.

If the flag DISABLE_COOKIES is set, then the packet needs to be sent as-is,
with no cookie attached.

********************************************************************/

/********************************************************************

Packet format for the messages:

       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |     Type      |     Code      |          Checksum             |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |  rsvd |lt_log2|             Reserved                          |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                                                               |
      +                                                               +
      |                  Echoed Cookie (96 bit)                       |
      +                                                               +
      |                                                               |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                                                               |
      +                                                               +
      |                  Requested Cookie (96 bit)                    |
      +                                                               +
      |                                                               |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

The ICMP message is therefore 8*4 = 32 bytes.

lt_log2: 4 bits log2 of the expected cookie lifetime.
         The receiver of a verified message should expect the
         next update of requested cookie in 2^lt_log2 seconds.

Echoed Cookie: The cookie echoed back in order to verify the source of
         the ICMP message.

Requested Cookie: Valid for SET-COOKIE, this is the value the sender
         wants to see in all the messages coming from the target of
         the ICMP message.

********************************************************************/

typedef struct icmp6_ipcookies {
  ipcookie_t echoed_cookie;
  ipcookie_t requested_cookie;
} icmp6_ipcookies_t;

#define IPCOOKIES_ICMP_SIZE (sizeof(struct icmp6_hdr) + sizeof(struct icmp6_ipcookies))

#define icmp6_ipck_lt_log2 icmp6_data8[0]
#define ICMP6_IPCK_LT_LOG2_MASK 0x0F

#define ICMP6_IPCOOKIES 0x42

#define ICMP6_IC_SET_COOKIE 0x1
#define ICMP6_IC_SETCOOKIE_NOT_EXPECTED 0x02


#define IPCOOKIES_PACKET_BUF_SIZE 1500



ipcookie_full_state_t *mmap_ipcookies(void);
void die_perror(char *msg);

void ipcookies_icmp_send(uint8_t code, ipcookie_t *echoed_cookie,
                         ipcookie_t *requested_cookie, struct in6_addr *icmp_dst_addr);

void ipcookie_entry_update_mtime(ipcookie_entry_t *ce);
void ipcookie_entry_set_lifetime_log2(ipcookie_entry_t *ce, int new_lifetime_log2);
void ipcookie_entry_mtime_backdate_by_lifetime_log2(ipcookie_entry_t *ce);


