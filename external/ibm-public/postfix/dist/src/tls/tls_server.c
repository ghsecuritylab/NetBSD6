/*	$NetBSD: tls_server.c,v 1.6 2013/01/02 19:18:36 tron Exp $	*/

/*++
/* NAME
/*	tls_server 3
/* SUMMARY
/*	server-side TLS engine
/* SYNOPSIS
/*	#include <tls.h>
/*
/*	TLS_APPL_STATE *tls_server_init(props)
/*	const TLS_SERVER_INIT_PROPS *props;
/*
/*	TLS_SESS_STATE *tls_server_start(props)
/*	const TLS_SERVER_START_PROPS *props;
/*
/*	TLS_SESS_STATE *tls_server_post_accept(TLScontext)
/*	TLS_SESS_STATE *TLScontext;
/*
/*	void	tls_server_stop(app_ctx, stream, failure, TLScontext)
/*	TLS_APPL_STATE *app_ctx;
/*	VSTREAM	*stream;
/*	int	failure;
/*	TLS_SESS_STATE *TLScontext;
/* DESCRIPTION
/*	This module is the interface between Postfix TLS servers,
/*	the OpenSSL library, and the TLS entropy and cache manager.
/*
/*	See "EVENT_DRIVEN APPLICATIONS" below for using this code
/*	in event-driven programs.
/*
/*	tls_server_init() is called once when the SMTP server
/*	initializes.
/*	Certificate details are also decided during this phase,
/*	so that peer-specific behavior is not possible.
/*
/*	tls_server_start() activates the TLS feature for the VSTREAM
/*	passed as argument. We assume that network buffers are flushed
/*	and the TLS handshake can begin	immediately.
/*
/*	tls_server_stop() sends the "close notify" alert via
/*	SSL_shutdown() to the peer and resets all connection specific
/*	TLS data. As RFC2487 does not specify a separate shutdown, it
/*	is assumed that the underlying TCP connection is shut down
/*	immediately afterwards. Any further writes to the channel will
/*	be discarded, and any further reads will report end-of-file.
/*	If the failure flag is set, no SSL_shutdown() handshake is performed.
/*
/*	Once the TLS connection is initiated, information about the TLS
/*	state is available via the TLScontext structure:
/* .IP TLScontext->protocol
/*	the protocol name (SSLv2, SSLv3, TLSv1),
/* .IP TLScontext->cipher_name
/*	the cipher name (e.g. RC4/MD5),
/* .IP TLScontext->cipher_usebits
/*	the number of bits actually used (e.g. 40),
/* .IP TLScontext->cipher_algbits
/*	the number of bits the algorithm is based on (e.g. 128).
/* .PP
/*	The last two values may differ from each other when export-strength
/*	encryption is used.
/*
/*	If the peer offered a certificate, part of the certificate data are
/*	available as:
/* .IP TLScontext->peer_status
/*	A bitmask field that records the status of the peer certificate
/*	verification. One or more of TLS_CERT_FLAG_PRESENT and
/*	TLS_CERT_FLAG_TRUSTED.
/* .IP TLScontext->peer_CN
/*	Extracted CommonName of the peer, or zero-length string
/*	when information could not be extracted.
/* .IP TLScontext->issuer_CN
/*	Extracted CommonName of the issuer, or zero-length string
/*	when information could not be extracted.
/* .IP TLScontext->peer_fingerprint
/*	Fingerprint of the certificate, or zero-length string when no peer
/*	certificate is available.
/* .PP
/*	If no peer certificate is presented the peer_status is set to 0.
/* EVENT_DRIVEN APPLICATIONS
/* .ad
/* .fi
/*	Event-driven programs manage multiple I/O channels.  Such
/*	programs cannot use the synchronous VSTREAM-over-TLS
/*	implementation that the current TLS library provides,
/*	including tls_server_stop() and the underlying tls_stream(3)
/*	and tls_bio_ops(3) routines.
/*
/*	With the current TLS library implementation, this means
/*	that the application is responsible for calling and retrying
/*	SSL_accept(), SSL_read(), SSL_write() and SSL_shutdown().
/*
/*	To maintain control over TLS I/O, an event-driven server
/*	invokes tls_server_start() with a null VSTREAM argument and
/*	with an fd argument that specifies the I/O file descriptor.
/*	Then, tls_server_start() performs all the necessary
/*	preparations before the TLS handshake and returns a partially
/*	populated TLS context. The event-driven application is then
/*	responsible for invoking SSL_accept(), and if successful,
/*	for invoking tls_server_post_accept() to finish the work
/*	that was started by tls_server_start(). In case of unrecoverable
/*	failure, tls_server_post_accept() destroys the TLS context
/*	and returns a null pointer value.
/* LICENSE
/* .ad
/* .fi
/*	This software is free. You can do with it whatever you want.
/*	The original author kindly requests that you acknowledge
/*	the use of his software.
/* AUTHOR(S)
/*	Originally written by:
/*	Lutz Jaenicke
/*	BTU Cottbus
/*	Allgemeine Elektrotechnik
/*	Universitaetsplatz 3-4
/*	D-03044 Cottbus, Germany
/*
/*	Updated by:
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*
/*	Victor Duchovni
/*	Morgan Stanley
/*--*/

/* System library. */

#include <sys_defs.h>

#ifdef USE_TLS
#include <unistd.h>
#include <string.h>

/* Utility library. */

#include <mymalloc.h>
#include <vstring.h>
#include <vstream.h>
#include <dict.h>
#include <stringops.h>
#include <msg.h>
#include <hex_code.h>
#include <iostuff.h>			/* non-blocking */

/* Global library. */

#include <mail_params.h>

/* TLS library. */

#include <tls_mgr.h>
#define TLS_INTERNAL
#include <tls.h>

#define STR(x)	vstring_str(x)
#define LEN(x)	VSTRING_LEN(x)

/* Application-specific. */

 /*
  * The session_id_context indentifies the service that created a session.
  * This information is used to distinguish between multiple TLS-based
  * servers running on the same server. We use the name of the mail system.
  */
static const char server_session_id_context[] = "Postfix/TLS";

/* get_server_session_cb - callback to retrieve session from server cache */

static SSL_SESSION *get_server_session_cb(SSL *ssl, unsigned char *session_id,
					          int session_id_length,
					          int *unused_copy)
{
    const char *myname = "get_server_session_cb";
    TLS_SESS_STATE *TLScontext;
    VSTRING *cache_id;
    VSTRING *session_data = vstring_alloc(2048);
    SSL_SESSION *session = 0;

    if ((TLScontext = SSL_get_ex_data(ssl, TLScontext_index)) == 0)
	msg_panic("%s: null TLScontext in session lookup callback", myname);

#define GEN_CACHE_ID(buf, id, len, service) \
    do { \
	buf = vstring_alloc(2 * (len + strlen(service))); \
	hex_encode(buf, (char *) (id), (len)); \
    	vstring_sprintf_append(buf, "&s=%s", (service)); \
    	vstring_sprintf_append(buf, "&l=%ld", (long) SSLeay()); \
    } while (0)


    GEN_CACHE_ID(cache_id, session_id, session_id_length, TLScontext->serverid);

    if (TLScontext->log_mask & TLS_LOG_CACHE)
	msg_info("%s: looking up session %s in %s cache", TLScontext->namaddr,
		 STR(cache_id), TLScontext->cache_type);

    /*
     * Load the session from cache and decode it.
     */
    if (tls_mgr_lookup(TLScontext->cache_type, STR(cache_id),
		       session_data) == TLS_MGR_STAT_OK) {
	session = tls_session_activate(STR(session_data), LEN(session_data));
	if (session && (TLScontext->log_mask & TLS_LOG_CACHE))
	    msg_info("%s: reloaded session %s from %s cache",
		     TLScontext->namaddr, STR(cache_id),
		     TLScontext->cache_type);
    }

    /*
     * Clean up.
     */
    vstring_free(cache_id);
    vstring_free(session_data);

    return (session);
}

/* uncache_session - remove session from internal & external cache */

static void uncache_session(SSL_CTX *ctx, TLS_SESS_STATE *TLScontext)
{
    VSTRING *cache_id;
    SSL_SESSION *session = SSL_get_session(TLScontext->con);

    SSL_CTX_remove_session(ctx, session);

    if (TLScontext->cache_type == 0)
	return;

    GEN_CACHE_ID(cache_id, session->session_id, session->session_id_length,
		 TLScontext->serverid);

    if (TLScontext->log_mask & TLS_LOG_CACHE)
	msg_info("%s: remove session %s from %s cache", TLScontext->namaddr,
		 STR(cache_id), TLScontext->cache_type);

    tls_mgr_delete(TLScontext->cache_type, STR(cache_id));
    vstring_free(cache_id);
}

/* new_server_session_cb - callback to save session to server cache */

static int new_server_session_cb(SSL *ssl, SSL_SESSION *session)
{
    const char *myname = "new_server_session_cb";
    VSTRING *cache_id;
    TLS_SESS_STATE *TLScontext;
    VSTRING *session_data;

    if ((TLScontext = SSL_get_ex_data(ssl, TLScontext_index)) == 0)
	msg_panic("%s: null TLScontext in new session callback", myname);

    GEN_CACHE_ID(cache_id, session->session_id, session->session_id_length,
		 TLScontext->serverid);

    if (TLScontext->log_mask & TLS_LOG_CACHE)
	msg_info("%s: save session %s to %s cache", TLScontext->namaddr,
		 STR(cache_id), TLScontext->cache_type);

    /*
     * Passivate and save the session state.
     */
    session_data = tls_session_passivate(session);
    if (session_data)
	tls_mgr_update(TLScontext->cache_type, STR(cache_id),
		       STR(session_data), LEN(session_data));

    /*
     * Clean up.
     */
    if (session_data)
	vstring_free(session_data);
    vstring_free(cache_id);
    SSL_SESSION_free(session);			/* 200502 */

    return (1);
}

/* tls_server_init - initialize the server-side TLS engine */

TLS_APPL_STATE *tls_server_init(const TLS_SERVER_INIT_PROPS *props)
{
    SSL_CTX *server_ctx;
    long    off = 0;
    int     verify_flags = SSL_VERIFY_NONE;
    int     cachable;
    int     protomask;
    TLS_APPL_STATE *app_ctx;
    const EVP_MD *md_alg;
    unsigned int md_len;
    int     log_mask;

    /*
     * Convert user loglevel to internal logmask.
     */
    log_mask = tls_log_mask(props->log_param, props->log_level);

    if (log_mask & TLS_LOG_VERBOSE)
	msg_info("initializing the server-side TLS engine");

    /*
     * Load (mostly cipher related) TLS-library internal main.cf parameters.
     */
    tls_param_init();

    /*
     * Detect mismatch between compile-time headers and run-time library.
     */
    tls_check_version();

    /*
     * Initialize the OpenSSL library by the book! To start with, we must
     * initialize the algorithms. We want cleartext error messages instead of
     * just error codes, so we load the error_strings.
     */
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    /*
     * First validate the protocols. If these are invalid, we can't continue.
     */
    protomask = tls_protocol_mask(props->protocols);
    if (protomask == TLS_PROTOCOL_INVALID) {
	/* tls_protocol_mask() logs no warning. */
	msg_warn("Invalid TLS protocol list \"%s\": disabling TLS support",
		 props->protocols);
	return (0);
    }

    /*
     * Create an application data index for SSL objects, so that we can
     * attach TLScontext information; this information is needed inside
     * tls_verify_certificate_callback().
     */
    if (TLScontext_index < 0) {
	if ((TLScontext_index = SSL_get_ex_new_index(0, 0, 0, 0, 0)) < 0) {
	    msg_warn("Cannot allocate SSL application data index: "
		     "disabling TLS support");
	    return (0);
	}
    }

    /*
     * If the administrator specifies an unsupported digest algorithm, fail
     * now, rather than in the middle of a TLS handshake.
     */
    if ((md_alg = EVP_get_digestbyname(props->fpt_dgst)) == 0) {
	msg_warn("Digest algorithm \"%s\" not found: disabling TLS support",
		 props->fpt_dgst);
	return (0);
    }

    /*
     * Sanity check: Newer shared libraries may use larger digests.
     */
    if ((md_len = EVP_MD_size(md_alg)) > EVP_MAX_MD_SIZE) {
	msg_warn("Digest algorithm \"%s\" output size %u too large:"
		 " disabling TLS support", props->fpt_dgst, md_len);
	return (0);
    }

    /*
     * Initialize the PRNG (Pseudo Random Number Generator) with some seed
     * from external and internal sources. Don't enable TLS without some real
     * entropy.
     */
    if (tls_ext_seed(var_tls_daemon_rand_bytes) < 0) {
	msg_warn("no entropy for TLS key generation: disabling TLS support");
	return (0);
    }
    tls_int_seed();

    /*
     * The SSL/TLS specifications require the client to send a message in the
     * oldest specification it understands with the highest level it
     * understands in the message. Netscape communicator can still
     * communicate with SSLv2 servers, so it sends out a SSLv2 client hello.
     * To deal with it, our server must be SSLv2 aware (even if we don't like
     * SSLv2), so we need to have the SSLv23 server here. If we want to limit
     * the protocol level, we can add an option to not use SSLv2/v3/TLSv1
     * later.
     */
    ERR_clear_error();
    if ((server_ctx = SSL_CTX_new(SSLv23_server_method())) == 0) {
	msg_warn("cannot allocate server SSL_CTX: disabling TLS support");
	tls_print_errors();
	return (0);
    }

    /*
     * See the verify callback in tls_verify.c
     */
    SSL_CTX_set_verify_depth(server_ctx, props->verifydepth + 1);

    /*
     * Protocol work-arounds, OpenSSL version dependent.
     */
    off |= tls_bug_bits();
    SSL_CTX_set_options(server_ctx, off);

    /*
     * Global protocol selection.
     */
    if (protomask != 0)
	SSL_CTX_set_options(server_ctx,
		   ((protomask & TLS_PROTOCOL_TLSv1) ? SSL_OP_NO_TLSv1 : 0L)
	     | ((protomask & TLS_PROTOCOL_TLSv1_1) ? SSL_OP_NO_TLSv1_1 : 0L)
	     | ((protomask & TLS_PROTOCOL_TLSv1_2) ? SSL_OP_NO_TLSv1_2 : 0L)
		 | ((protomask & TLS_PROTOCOL_SSLv3) ? SSL_OP_NO_SSLv3 : 0L)
	       | ((protomask & TLS_PROTOCOL_SSLv2) ? SSL_OP_NO_SSLv2 : 0L));

#if OPENSSL_VERSION_NUMBER >= 0x0090700fL

    /*
     * Some sites may want to give the client less rope. On the other hand,
     * this could trigger inter-operability issues, the client should not
     * offer ciphers it implements poorly, but this hasn't stopped some
     * vendors from getting it wrong.
     * 
     * XXX: Given OpenSSL's security history, nobody should still be using
     * 0.9.7, let alone 0.9.6 or earlier. Warning added to TLS_README.html.
     */
    if (var_tls_preempt_clist)
	SSL_CTX_set_options(server_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
#endif

    /*
     * Set the call-back routine to debug handshake progress.
     */
    if (log_mask & TLS_LOG_DEBUG)
	SSL_CTX_set_info_callback(server_ctx, tls_info_callback);

    /*
     * Load the CA public key certificates for both the server cert and for
     * the verification of client certificates. As provided by OpenSSL we
     * support two types of CA certificate handling: One possibility is to
     * add all CA certificates to one large CAfile, the other possibility is
     * a directory pointed to by CApath, containing separate files for each
     * CA with softlinks named after the hash values of the certificate. The
     * first alternative has the advantage that the file is opened and read
     * at startup time, so that you don't have the hassle to maintain another
     * copy of the CApath directory for chroot-jail.
     */
    if (tls_set_ca_certificate_info(server_ctx,
				    props->CAfile, props->CApath) < 0) {
	/* tls_set_ca_certificate_info() already logs a warning. */
	SSL_CTX_free(server_ctx);		/* 200411 */
	return (0);
    }

    /*
     * Load the server public key certificate and private key from file and
     * check whether the cert matches the key. We can use RSA certificates
     * ("cert") DSA certificates ("dcert") or ECDSA certificates ("eccert").
     * All three can be made available at the same time. The CA certificates
     * for all three are handled in the same setup already finished. Which
     * one is used depends on the cipher negotiated (that is: the first
     * cipher listed by the client which does match the server). A client
     * with RSA only (e.g. Netscape) will use the RSA certificate only. A
     * client with openssl-library will use RSA first if not especially
     * changed in the cipher setup.
     */
    if (tls_set_my_certificate_key_info(server_ctx,
					props->cert_file,
					props->key_file,
					props->dcert_file,
					props->dkey_file,
					props->eccert_file,
					props->eckey_file) < 0) {
	/* tls_set_my_certificate_key_info() already logs a warning. */
	SSL_CTX_free(server_ctx);		/* 200411 */
	return (0);
    }

    /*
     * According to OpenSSL documentation, a temporary RSA key is needed when
     * export ciphers are in use, because the certified key cannot be
     * directly used.
     */
    SSL_CTX_set_tmp_rsa_callback(server_ctx, tls_tmp_rsa_cb);

    /*
     * Diffie-Hellman key generation parameters can either be loaded from
     * files (preferred) or taken from compiled in values. First, set the
     * callback that will select the values when requested, then load the
     * (possibly) available DH parameters from files. We are generous with
     * the error handling, since we do have default values compiled in, so we
     * will not abort but just log the error message.
     */
    SSL_CTX_set_tmp_dh_callback(server_ctx, tls_tmp_dh_cb);
    if (*props->dh1024_param_file != 0)
	tls_set_dh_from_file(props->dh1024_param_file, 1024);
    if (*props->dh512_param_file != 0)
	tls_set_dh_from_file(props->dh512_param_file, 512);

    /*
     * Enable EECDH if available, errors are not fatal, we just keep going
     * with any remaining key-exchange algorithms.
     */
    (void) tls_set_eecdh_curve(server_ctx, props->eecdh_grade);

    /*
     * If we want to check client certificates, we have to indicate it in
     * advance. By now we only allow to decide on a global basis. If we want
     * to allow certificate based relaying, we must ask the client to provide
     * one with SSL_VERIFY_PEER. The client now can decide, whether it
     * provides one or not. We can enforce a failure of the negotiation with
     * SSL_VERIFY_FAIL_IF_NO_PEER_CERT, if we do not allow a connection
     * without one. In the "server hello" following the initialization by the
     * "client hello" the server must provide a list of CAs it is willing to
     * accept. Some clever clients will then select one from the list of
     * available certificates matching these CAs. Netscape Communicator will
     * present the list of certificates for selecting the one to be sent, or
     * it will issue a warning, if there is no certificate matching the
     * available CAs.
     * 
     * With regard to the purpose of the certificate for relaying, we might like
     * a later negotiation, maybe relaying would already be allowed for other
     * reasons, but this would involve severe changes in the internal postfix
     * logic, so we have to live with it the way it is.
     */
    if (props->ask_ccert)
	verify_flags = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
    SSL_CTX_set_verify(server_ctx, verify_flags,
		       tls_verify_certificate_callback);
    if (*props->CAfile)
	SSL_CTX_set_client_CA_list(server_ctx,
				   SSL_load_client_CA_file(props->CAfile));

    /*
     * Initialize our own TLS server handle, before diving into the details
     * of TLS session cache management.
     */
    app_ctx = tls_alloc_app_context(server_ctx, log_mask);

    /*
     * The session cache is implemented by the tlsmgr(8) server.
     * 
     * XXX 200502 Surprise: when OpenSSL purges an entry from the in-memory
     * cache, it also attempts to purge the entry from the on-disk cache.
     * This is undesirable, especially when we set the in-memory cache size
     * to 1. For this reason we don't allow OpenSSL to purge on-disk cache
     * entries, and leave it up to the tlsmgr process instead. Found by
     * Victor Duchovni.
     */

    if (tls_mgr_policy(props->cache_type, &cachable) != TLS_MGR_STAT_OK)
	cachable = 0;

    if (cachable || props->set_sessid) {

	/*
	 * Initialize the session cache.
	 * 
	 * With a large number of concurrent smtpd(8) processes, it is not a
	 * good idea to cache multiple large session objects in each process.
	 * We set the internal cache size to 1, and don't register a
	 * "remove_cb" so as to avoid deleting good sessions from the
	 * external cache prematurely (when the internal cache is full,
	 * OpenSSL removes sessions from the external cache also)!
	 * 
	 * This makes SSL_CTX_remove_session() not useful for flushing broken
	 * sessions from the external cache, so we must delete them directly
	 * (not via a callback).
	 * 
	 * Set a session id context to identify to what type of server process
	 * created a session. In our case, the context is simply the name of
	 * the mail system: "Postfix/TLS".
	 */
	SSL_CTX_sess_set_cache_size(server_ctx, 1);
	SSL_CTX_set_session_id_context(server_ctx,
				       (void *) &server_session_id_context,
				       sizeof(server_session_id_context));
	SSL_CTX_set_session_cache_mode(server_ctx,
				       SSL_SESS_CACHE_SERVER |
				       SSL_SESS_CACHE_NO_AUTO_CLEAR);
	if (cachable) {
	    app_ctx->cache_type = mystrdup(props->cache_type);

	    SSL_CTX_sess_set_get_cb(server_ctx, get_server_session_cb);
	    SSL_CTX_sess_set_new_cb(server_ctx, new_server_session_cb);
	}

	/*
	 * OpenSSL ignores timed-out sessions. We need to set the internal
	 * cache timeout at least as high as the external cache timeout. This
	 * applies even if no internal cache is used.
	 */
	SSL_CTX_set_timeout(server_ctx, props->scache_timeout);
    } else {

	/*
	 * If we have no external cache, disable all caching. No use wasting
	 * server memory resources with sessions they are unlikely to be able
	 * to reuse.
	 */
	SSL_CTX_set_session_cache_mode(server_ctx, SSL_SESS_CACHE_OFF);
    }

    return (app_ctx);
}

 /*
  * This is the actual startup routine for a new connection. We expect that
  * the SMTP buffers are flushed and the "220 Ready to start TLS" was sent to
  * the client, so that we can immediately start the TLS handshake process.
  */
TLS_SESS_STATE *tls_server_start(const TLS_SERVER_START_PROPS *props)
{
    int     sts;
    TLS_SESS_STATE *TLScontext;
    const char *cipher_list;
    TLS_APPL_STATE *app_ctx = props->ctx;
    int     log_mask = app_ctx->log_mask;

    /*
     * Implicitly enable logging of trust chain errors when verified certs
     * are required.
     */
    if (props->requirecert)
	log_mask |= TLS_LOG_UNTRUSTED;

    if (log_mask & TLS_LOG_VERBOSE)
	msg_info("setting up TLS connection from %s", props->namaddr);

    cipher_list = tls_set_ciphers(app_ctx, "TLS", props->cipher_grade,
				  props->cipher_exclusions);
    if (cipher_list == 0) {
	msg_warn("%s: %s: aborting TLS session", props->namaddr,
		 vstring_str(app_ctx->why));
	return (0);
    }
    if (log_mask & TLS_LOG_VERBOSE)
	msg_info("%s: TLS cipher list \"%s\"", props->namaddr, cipher_list);

    /*
     * Allocate a new TLScontext for the new connection and get an SSL
     * structure. Add the location of TLScontext to the SSL to later retrieve
     * the information inside the tls_verify_certificate_callback().
     */
    TLScontext = tls_alloc_sess_context(log_mask, props->namaddr);
    TLScontext->cache_type = app_ctx->cache_type;

    TLScontext->serverid = mystrdup(props->serverid);
    TLScontext->am_server = 1;

    TLScontext->fpt_dgst = mystrdup(props->fpt_dgst);
    TLScontext->stream = props->stream;

    ERR_clear_error();
    if ((TLScontext->con = (SSL *) SSL_new(app_ctx->ssl_ctx)) == 0) {
	msg_warn("Could not allocate 'TLScontext->con' with SSL_new()");
	tls_print_errors();
	tls_free_context(TLScontext);
	return (0);
    }
    if (!SSL_set_ex_data(TLScontext->con, TLScontext_index, TLScontext)) {
	msg_warn("Could not set application data for 'TLScontext->con'");
	tls_print_errors();
	tls_free_context(TLScontext);
	return (0);
    }

    /*
     * Before really starting anything, try to seed the PRNG a little bit
     * more.
     */
    tls_int_seed();
    (void) tls_ext_seed(var_tls_daemon_rand_bytes);

    /*
     * Initialize the SSL connection to accept state. This should not be
     * necessary anymore since 0.9.3, but the call is still in the library
     * and maintaining compatibility never hurts.
     */
    SSL_set_accept_state(TLScontext->con);

    /*
     * Connect the SSL connection with the network socket.
     */
    if (SSL_set_fd(TLScontext->con, props->stream == 0 ? props->fd :
		   vstream_fileno(props->stream)) != 1) {
	msg_info("SSL_set_fd error to %s", props->namaddr);
	tls_print_errors();
	uncache_session(app_ctx->ssl_ctx, TLScontext);
	tls_free_context(TLScontext);
	return (0);
    }

    /*
     * If the debug level selected is high enough, all of the data is dumped:
     * TLS_LOG_TLSPKTS will dump the SSL negotiation, TLS_LOG_ALLPKTS will
     * dump everything.
     * 
     * We do have an SSL_set_fd() and now suddenly a BIO_ routine is called?
     * Well there is a BIO below the SSL routines that is automatically
     * created for us, so we can use it for debugging purposes.
     */
    if (log_mask & TLS_LOG_TLSPKTS)
	BIO_set_callback(SSL_get_rbio(TLScontext->con), tls_bio_dump_cb);

    /*
     * If we don't trigger the handshake in the library, leave control over
     * SSL_accept/read/write/etc with the application.
     */
    if (props->stream == 0)
	return (TLScontext);

    /*
     * Turn on non-blocking I/O so that we can enforce timeouts on network
     * I/O.
     */
    non_blocking(vstream_fileno(props->stream), NON_BLOCKING);

    /*
     * Start TLS negotiations. This process is a black box that invokes our
     * call-backs for session caching and certificate verification.
     * 
     * Error handling: If the SSL handhake fails, we print out an error message
     * and remove all TLS state concerning this session.
     */
    sts = tls_bio_accept(vstream_fileno(props->stream), props->timeout,
			 TLScontext);
    if (sts <= 0) {
	if (ERR_peek_error() != 0) {
	    msg_info("SSL_accept error from %s: %d", props->namaddr, sts);
	    tls_print_errors();
	} else if (errno != 0) {
	    msg_info("SSL_accept error from %s: %m", props->namaddr);
	} else {
	    msg_info("SSL_accept error from %s: lost connection",
		     props->namaddr);
	}
	tls_free_context(TLScontext);
	return (0);
    }
    return (tls_server_post_accept(TLScontext));
}

/* tls_server_post_accept - post-handshake processing */

TLS_SESS_STATE *tls_server_post_accept(TLS_SESS_STATE *TLScontext)
{
    const SSL_CIPHER *cipher;
    X509   *peer;
    char    buf[CCERT_BUFSIZ];

    /* Turn off packet dump if only dumping the handshake */
    if ((TLScontext->log_mask & TLS_LOG_ALLPKTS) == 0)
	BIO_set_callback(SSL_get_rbio(TLScontext->con), 0);

    /*
     * The caller may want to know if this session was reused or if a new
     * session was negotiated.
     */
    TLScontext->session_reused = SSL_session_reused(TLScontext->con);
    if ((TLScontext->log_mask & TLS_LOG_CACHE) && TLScontext->session_reused)
	msg_info("%s: Reusing old session", TLScontext->namaddr);

    /*
     * Let's see whether a peer certificate is available and what is the
     * actual information. We want to save it for later use.
     */
    peer = SSL_get_peer_certificate(TLScontext->con);
    if (peer != NULL) {
	TLScontext->peer_status |= TLS_CERT_FLAG_PRESENT;
	if (SSL_get_verify_result(TLScontext->con) == X509_V_OK)
	    TLScontext->peer_status |= TLS_CERT_FLAG_TRUSTED;

	if (TLScontext->log_mask & TLS_LOG_VERBOSE) {
	    X509_NAME_oneline(X509_get_subject_name(peer),
			      buf, sizeof(buf));
	    msg_info("subject=%s", buf);
	    X509_NAME_oneline(X509_get_issuer_name(peer),
			      buf, sizeof(buf));
	    msg_info("issuer=%s", buf);
	}
	TLScontext->peer_CN = tls_peer_CN(peer, TLScontext);
	TLScontext->issuer_CN = tls_issuer_CN(peer, TLScontext);
	TLScontext->peer_fingerprint =
	    tls_fingerprint(peer, TLScontext->fpt_dgst);
	TLScontext->peer_pkey_fprint =
	    tls_pkey_fprint(peer, TLScontext->fpt_dgst);

	if (TLScontext->log_mask & (TLS_LOG_VERBOSE | TLS_LOG_PEERCERT)) {
	    msg_info("%s: subject_CN=%s, issuer=%s, fingerprint=%s"
		     ", pkey_fingerprint=%s",
		     TLScontext->namaddr,
		     TLScontext->peer_CN, TLScontext->issuer_CN,
		     TLScontext->peer_fingerprint,
		     TLScontext->peer_pkey_fprint);
	}
	X509_free(peer);
    } else {
	TLScontext->peer_CN = mystrdup("");
	TLScontext->issuer_CN = mystrdup("");
	TLScontext->peer_fingerprint = mystrdup("");
    }

    /*
     * Finally, collect information about protocol and cipher for logging
     */
    TLScontext->protocol = SSL_get_version(TLScontext->con);
    cipher = SSL_get_current_cipher(TLScontext->con);
    TLScontext->cipher_name = SSL_CIPHER_get_name(cipher);
    TLScontext->cipher_usebits = SSL_CIPHER_get_bits(cipher,
					     &(TLScontext->cipher_algbits));

    /*
     * If the library triggered the SSL handshake, switch to the
     * tls_timed_read/write() functions and make the TLScontext available to
     * those functions. Otherwise, leave control over SSL_read/write/etc.
     * with the application.
     */
    if (TLScontext->stream != 0)
	tls_stream_start(TLScontext->stream, TLScontext);

    /*
     * All the key facts in a single log entry.
     */
    if (TLScontext->log_mask & TLS_LOG_SUMMARY)
	msg_info("%s TLS connection established from %s: %s with cipher %s "
	      "(%d/%d bits)", !TLS_CERT_IS_PRESENT(TLScontext) ? "Anonymous"
		 : TLS_CERT_IS_TRUSTED(TLScontext) ? "Trusted" : "Untrusted",
	 TLScontext->namaddr, TLScontext->protocol, TLScontext->cipher_name,
		 TLScontext->cipher_usebits, TLScontext->cipher_algbits);

    tls_int_seed();

    return (TLScontext);
}

#endif					/* USE_TLS */
