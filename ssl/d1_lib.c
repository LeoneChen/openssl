/* ssl/d1_lib.c */
/*
 * DTLS implementation written by Nagendra Modadugu
 * (nagendra@cs.stanford.edu) for the OpenSSL project 2005.
 */
/* ====================================================================
 * Copyright (c) 1999-2005 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>
#define USE_SOCKETS
#include <openssl/objects.h>
#include "ssl_locl.h"

#if defined(OPENSSL_SYS_VMS)
# include <sys/timeb.h>
#elif defined(OPENSSL_SYS_NETWARE) && !defined(_WINSOCK2API_)
# include <sys/timeval.h>
#elif defined(OPENSSL_SYS_VXWORKS)
# include <sys/times.h>
#elif !defined(OPENSSL_SYS_WIN32)
# include <sys/time.h>
#endif

static void get_current_time(struct timeval *t);
static int dtls1_set_handshake_header(SSL *s, int type, unsigned long len);
static int dtls1_handshake_write(SSL *s);
const char dtls1_version_str[] = "DTLSv1" OPENSSL_VERSION_PTEXT;
int dtls1_listen(SSL *s, struct sockaddr *client);

const SSL3_ENC_METHOD DTLSv1_enc_data = {
    tls1_enc,
    tls1_mac,
    tls1_setup_key_block,
    tls1_generate_master_secret,
    tls1_change_cipher_state,
    tls1_final_finish_mac,
    TLS1_FINISH_MAC_LENGTH,
    tls1_cert_verify_mac,
    TLS_MD_CLIENT_FINISH_CONST, TLS_MD_CLIENT_FINISH_CONST_SIZE,
    TLS_MD_SERVER_FINISH_CONST, TLS_MD_SERVER_FINISH_CONST_SIZE,
    tls1_alert_code,
    tls1_export_keying_material,
    SSL_ENC_FLAG_DTLS | SSL_ENC_FLAG_EXPLICIT_IV,
    DTLS1_HM_HEADER_LENGTH,
    dtls1_set_handshake_header,
    dtls1_handshake_write
};

const SSL3_ENC_METHOD DTLSv1_2_enc_data = {
    tls1_enc,
    tls1_mac,
    tls1_setup_key_block,
    tls1_generate_master_secret,
    tls1_change_cipher_state,
    tls1_final_finish_mac,
    TLS1_FINISH_MAC_LENGTH,
    tls1_cert_verify_mac,
    TLS_MD_CLIENT_FINISH_CONST, TLS_MD_CLIENT_FINISH_CONST_SIZE,
    TLS_MD_SERVER_FINISH_CONST, TLS_MD_SERVER_FINISH_CONST_SIZE,
    tls1_alert_code,
    tls1_export_keying_material,
    SSL_ENC_FLAG_DTLS | SSL_ENC_FLAG_EXPLICIT_IV | SSL_ENC_FLAG_SIGALGS
        | SSL_ENC_FLAG_SHA256_PRF | SSL_ENC_FLAG_TLS1_2_CIPHERS,
    DTLS1_HM_HEADER_LENGTH,
    dtls1_set_handshake_header,
    dtls1_handshake_write
};

long dtls1_default_timeout(void)
{
    /*
     * 2 hours, the 24 hours mentioned in the DTLSv1 spec is way too long for
     * http, the cache would over fill
     */
    return (60 * 60 * 2);
}

int dtls1_new(SSL *s)
{
    DTLS1_STATE *d1;

    if (!DTLS_RECORD_LAYER_new(&s->rlayer)) {
        return 0;
    }
    
    if (!ssl3_new(s))
        return (0);
    if ((d1 = OPENSSL_malloc(sizeof(*d1))) == NULL) {
        ssl3_free(s);
        return (0);
    }
    memset(d1, 0, sizeof(*d1));

    d1->buffered_messages = pqueue_new();
    d1->sent_messages = pqueue_new();

    if (s->server) {
        d1->cookie_len = sizeof(s->d1->cookie);
    }

    d1->link_mtu = 0;
    d1->mtu = 0;

    if (!d1->buffered_messages || !d1->sent_messages) {
        pqueue_free(d1->buffered_messages);
        pqueue_free(d1->sent_messages);
        OPENSSL_free(d1);
        ssl3_free(s);
        return (0);
    }

    s->d1 = d1;
    s->method->ssl_clear(s);
    return (1);
}

static void dtls1_clear_queues(SSL *s)
{
    pitem *item = NULL;
    hm_fragment *frag = NULL;

    while ((item = pqueue_pop(s->d1->buffered_messages)) != NULL) {
        frag = (hm_fragment *)item->data;
        dtls1_hm_fragment_free(frag);
        pitem_free(item);
    }

    while ((item = pqueue_pop(s->d1->sent_messages)) != NULL) {
        frag = (hm_fragment *)item->data;
        dtls1_hm_fragment_free(frag);
        pitem_free(item);
    }
}

void dtls1_free(SSL *s)
{
    DTLS_RECORD_LAYER_free(&s->rlayer);

    ssl3_free(s);

    dtls1_clear_queues(s);

    pqueue_free(s->d1->buffered_messages);
    pqueue_free(s->d1->sent_messages);

    OPENSSL_free(s->d1);
    s->d1 = NULL;
}

void dtls1_clear(SSL *s)
{
    pqueue buffered_messages;
    pqueue sent_messages;
    unsigned int mtu;
    unsigned int link_mtu;

    DTLS_RECORD_LAYER_clear(&s->rlayer);

    if (s->d1) {
        buffered_messages = s->d1->buffered_messages;
        sent_messages = s->d1->sent_messages;
        mtu = s->d1->mtu;
        link_mtu = s->d1->link_mtu;

        dtls1_clear_queues(s);

        memset(s->d1, 0, sizeof(*s->d1));

        if (s->server) {
            s->d1->cookie_len = sizeof(s->d1->cookie);
        }

        if (SSL_get_options(s) & SSL_OP_NO_QUERY_MTU) {
            s->d1->mtu = mtu;
            s->d1->link_mtu = link_mtu;
        }

        s->d1->buffered_messages = buffered_messages;
        s->d1->sent_messages = sent_messages;
    }

    ssl3_clear(s);
    if (s->options & SSL_OP_CISCO_ANYCONNECT)
        s->client_version = s->version = DTLS1_BAD_VER;
    else if (s->method->version == DTLS_ANY_VERSION)
        s->version = DTLS1_2_VERSION;
    else
        s->version = s->method->version;
}

long dtls1_ctrl(SSL *s, int cmd, long larg, void *parg)
{
    int ret = 0;

    switch (cmd) {
    case DTLS_CTRL_GET_TIMEOUT:
        if (dtls1_get_timeout(s, (struct timeval *)parg) != NULL) {
            ret = 1;
        }
        break;
    case DTLS_CTRL_HANDLE_TIMEOUT:
        ret = dtls1_handle_timeout(s);
        break;
    case DTLS_CTRL_LISTEN:
        ret = dtls1_listen(s, parg);
        break;
    case SSL_CTRL_CHECK_PROTO_VERSION:
        /*
         * For library-internal use; checks that the current protocol is the
         * highest enabled version (according to s->ctx->method, as version
         * negotiation may have changed s->method).
         */
        if (s->version == s->ctx->method->version)
            return 1;
        /*
         * Apparently we're using a version-flexible SSL_METHOD (not at its
         * highest protocol version).
         */
        if (s->ctx->method->version == DTLS_method()->version) {
#if DTLS_MAX_VERSION != DTLS1_2_VERSION
# error Code needs update for DTLS_method() support beyond DTLS1_2_VERSION.
#endif
            if (!(s->options & SSL_OP_NO_DTLSv1_2))
                return s->version == DTLS1_2_VERSION;
            if (!(s->options & SSL_OP_NO_DTLSv1))
                return s->version == DTLS1_VERSION;
        }
        return 0;               /* Unexpected state; fail closed. */
    case DTLS_CTRL_SET_LINK_MTU:
        if (larg < (long)dtls1_link_min_mtu())
            return 0;
        s->d1->link_mtu = larg;
        return 1;
    case DTLS_CTRL_GET_LINK_MIN_MTU:
        return (long)dtls1_link_min_mtu();
    case SSL_CTRL_SET_MTU:
        /*
         *  We may not have a BIO set yet so can't call dtls1_min_mtu()
         *  We'll have to make do with dtls1_link_min_mtu() and max overhead
         */
        if (larg < (long)dtls1_link_min_mtu() - DTLS1_MAX_MTU_OVERHEAD)
            return 0;
        s->d1->mtu = larg;
        return larg;
    default:
        ret = ssl3_ctrl(s, cmd, larg, parg);
        break;
    }
    return (ret);
}

/*
 * As it's impossible to use stream ciphers in "datagram" mode, this
 * simple filter is designed to disengage them in DTLS. Unfortunately
 * there is no universal way to identify stream SSL_CIPHER, so we have
 * to explicitly list their SSL_* codes. Currently RC4 is the only one
 * available, but if new ones emerge, they will have to be added...
 */
const SSL_CIPHER *dtls1_get_cipher(unsigned int u)
{
    const SSL_CIPHER *ciph = ssl3_get_cipher(u);

    if (ciph != NULL) {
        if (ciph->algorithm_enc == SSL_RC4)
            return NULL;
    }

    return ciph;
}

void dtls1_start_timer(SSL *s)
{
#ifndef OPENSSL_NO_SCTP
    /* Disable timer for SCTP */
    if (BIO_dgram_is_sctp(SSL_get_wbio(s))) {
        memset(&s->d1->next_timeout, 0, sizeof(s->d1->next_timeout));
        return;
    }
#endif

    /* If timer is not set, initialize duration with 1 second */
    if (s->d1->next_timeout.tv_sec == 0 && s->d1->next_timeout.tv_usec == 0) {
        s->d1->timeout_duration = 1;
    }

    /* Set timeout to current time */
    get_current_time(&(s->d1->next_timeout));

    /* Add duration to current time */
    s->d1->next_timeout.tv_sec += s->d1->timeout_duration;
    BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT, 0,
             &(s->d1->next_timeout));
}

struct timeval *dtls1_get_timeout(SSL *s, struct timeval *timeleft)
{
    struct timeval timenow;

    /* If no timeout is set, just return NULL */
    if (s->d1->next_timeout.tv_sec == 0 && s->d1->next_timeout.tv_usec == 0) {
        return NULL;
    }

    /* Get current time */
    get_current_time(&timenow);

    /* If timer already expired, set remaining time to 0 */
    if (s->d1->next_timeout.tv_sec < timenow.tv_sec ||
        (s->d1->next_timeout.tv_sec == timenow.tv_sec &&
         s->d1->next_timeout.tv_usec <= timenow.tv_usec)) {
        memset(timeleft, 0, sizeof(*timeleft));
        return timeleft;
    }

    /* Calculate time left until timer expires */
    memcpy(timeleft, &(s->d1->next_timeout), sizeof(struct timeval));
    timeleft->tv_sec -= timenow.tv_sec;
    timeleft->tv_usec -= timenow.tv_usec;
    if (timeleft->tv_usec < 0) {
        timeleft->tv_sec--;
        timeleft->tv_usec += 1000000;
    }

    /*
     * If remaining time is less than 15 ms, set it to 0 to prevent issues
     * because of small devergences with socket timeouts.
     */
    if (timeleft->tv_sec == 0 && timeleft->tv_usec < 15000) {
        memset(timeleft, 0, sizeof(*timeleft));
    }

    return timeleft;
}

int dtls1_is_timer_expired(SSL *s)
{
    struct timeval timeleft;

    /* Get time left until timeout, return false if no timer running */
    if (dtls1_get_timeout(s, &timeleft) == NULL) {
        return 0;
    }

    /* Return false if timer is not expired yet */
    if (timeleft.tv_sec > 0 || timeleft.tv_usec > 0) {
        return 0;
    }

    /* Timer expired, so return true */
    return 1;
}

void dtls1_double_timeout(SSL *s)
{
    s->d1->timeout_duration *= 2;
    if (s->d1->timeout_duration > 60)
        s->d1->timeout_duration = 60;
    dtls1_start_timer(s);
}

void dtls1_stop_timer(SSL *s)
{
    /* Reset everything */
    memset(&s->d1->timeout, 0, sizeof(s->d1->timeout));
    memset(&s->d1->next_timeout, 0, sizeof(s->d1->next_timeout));
    s->d1->timeout_duration = 1;
    BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT, 0,
             &(s->d1->next_timeout));
    /* Clear retransmission buffer */
    dtls1_clear_record_buffer(s);
}

int dtls1_check_timeout_num(SSL *s)
{
    unsigned int mtu;

    s->d1->timeout.num_alerts++;

    /* Reduce MTU after 2 unsuccessful retransmissions */
    if (s->d1->timeout.num_alerts > 2
        && !(SSL_get_options(s) & SSL_OP_NO_QUERY_MTU)) {
        mtu =
            BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_GET_FALLBACK_MTU, 0,
                     NULL);
        if (mtu < s->d1->mtu)
            s->d1->mtu = mtu;
    }

    if (s->d1->timeout.num_alerts > DTLS1_TMO_ALERT_COUNT) {
        /* fail the connection, enough alerts have been sent */
        SSLerr(SSL_F_DTLS1_CHECK_TIMEOUT_NUM, SSL_R_READ_TIMEOUT_EXPIRED);
        return -1;
    }

    return 0;
}

int dtls1_handle_timeout(SSL *s)
{
    /* if no timer is expired, don't do anything */
    if (!dtls1_is_timer_expired(s)) {
        return 0;
    }

    dtls1_double_timeout(s);

    if (dtls1_check_timeout_num(s) < 0)
        return -1;

    s->d1->timeout.read_timeouts++;
    if (s->d1->timeout.read_timeouts > DTLS1_TMO_READ_COUNT) {
        s->d1->timeout.read_timeouts = 1;
    }
#ifndef OPENSSL_NO_HEARTBEATS
    if (s->tlsext_hb_pending) {
        s->tlsext_hb_pending = 0;
        return dtls1_heartbeat(s);
    }
#endif

    dtls1_start_timer(s);
    return dtls1_retransmit_buffered_messages(s);
}

static void get_current_time(struct timeval *t)
{
    sgx_get_current_time(t);
    return;
#if defined(_WIN32)
    SYSTEMTIME st;
    union {
        unsigned __int64 ul;
        FILETIME ft;
    } now;

    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &now.ft);
# ifdef  __MINGW32__
    now.ul -= 116444736000000000ULL;
# else
    now.ul -= 116444736000000000UI64; /* re-bias to 1/1/1970 */
# endif
    t->tv_sec = (long)(now.ul / 10000000);
    t->tv_usec = ((int)(now.ul % 10000000)) / 10;
#elif defined(OPENSSL_SYS_VMS)
    struct timeb tb;
    ftime(&tb);
    t->tv_sec = (long)tb.time;
    t->tv_usec = (long)tb.millitm * 1000;
#else
    gettimeofday(t, NULL);
#endif
}

int dtls1_listen(SSL *s, struct sockaddr *client)
{
    int ret;

    /* Ensure there is no state left over from a previous invocation */
    if (!SSL_clear(s))
        return -1;

    SSL_set_options(s, SSL_OP_COOKIE_EXCHANGE);
    s->d1->listen = 1;

    ret = SSL_accept(s);
    if (ret <= 0)
        return ret;

    (void)BIO_dgram_get_peer(SSL_get_rbio(s), client);
    return 1;
}

static int dtls1_set_handshake_header(SSL *s, int htype, unsigned long len)
{
    unsigned char *p = (unsigned char *)s->init_buf->data;
    dtls1_set_message_header(s, p, htype, len, 0, len);
    s->init_num = (int)len + DTLS1_HM_HEADER_LENGTH;
    s->init_off = 0;
    /* Buffer the message to handle re-xmits */

    if (!dtls1_buffer_message(s, 0))
        return 0;

    return 1;
}

static int dtls1_handshake_write(SSL *s)
{
    return dtls1_do_write(s, SSL3_RT_HANDSHAKE);
}
