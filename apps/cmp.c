/*
 * Copyright 2007-2020 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright Nokia 2007-2019
 * Copyright Siemens AG 2015-2019
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>
#include <ctype.h>

#include "apps.h"
#include "http_server.h"
#include "s_apps.h"
#include "progs.h"

#include "cmp_mock_srv.h"

/* tweaks needed due to missing unistd.h on Windows */
#ifdef _WIN32
# define access _access
#endif
#ifndef F_OK
# define F_OK 0
#endif

#include <openssl/ui.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>

/* explicit #includes not strictly needed since implied by the above: */
#include <stdlib.h>
#include <openssl/cmp.h>
#include <openssl/cmp_util.h>
#include <openssl/crmf.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/store.h>
#include <openssl/objects.h>
#include <openssl/x509.h>

DEFINE_STACK_OF(X509)
DEFINE_STACK_OF(X509_EXTENSION)
DEFINE_STACK_OF(OSSL_CMP_ITAV)

static char *opt_config = NULL;
#define CMP_SECTION "cmp"
#define SECTION_NAME_MAX 40 /* max length of section name */
#define DEFAULT_SECTION "default"
static char *opt_section = CMP_SECTION;

#undef PROG
#define PROG cmp_main
static char *prog = "cmp";

static int read_config(void);

static CONF *conf = NULL; /* OpenSSL config file context structure */
static OSSL_CMP_CTX *cmp_ctx = NULL; /* the client-side CMP context */

/* the type of cmp command we want to send */
typedef enum {
    CMP_IR,
    CMP_KUR,
    CMP_CR,
    CMP_P10CR,
    CMP_RR,
    CMP_GENM
} cmp_cmd_t;

/* message transfer */
static char *opt_server = NULL;
static char server_port_s[32] = { '\0' };
static int server_port = 0;
static char *opt_proxy = NULL;
static char *opt_no_proxy = NULL;
static char *opt_path = "/";
static int opt_msg_timeout = -1;
static int opt_total_timeout = -1;

/* server authentication */
static char *opt_trusted = NULL;
static char *opt_untrusted = NULL;
static char *opt_srvcert = NULL;
static char *opt_recipient = NULL;
static char *opt_expect_sender = NULL;
static int opt_ignore_keyusage = 0;
static int opt_unprotected_errors = 0;
static char *opt_extracertsout = NULL;
static char *opt_cacertsout = NULL;

/* client authentication */
static char *opt_ref = NULL;
static char *opt_secret = NULL;
static char *opt_cert = NULL;
static char *opt_key = NULL;
static char *opt_keypass = NULL;
static char *opt_digest = NULL;
static char *opt_mac = NULL;
static char *opt_extracerts = NULL;
static int opt_unprotected_requests = 0;

/* generic message */
static char *opt_cmd_s = NULL;
static int opt_cmd = -1;
static char *opt_geninfo = NULL;
static char *opt_infotype_s = NULL;
static int opt_infotype = NID_undef;

/* certificate enrollment */
static char *opt_newkey = NULL;
static char *opt_newkeypass = NULL;
static char *opt_subject = NULL;
static char *opt_issuer = NULL;
static int opt_days = 0;
static char *opt_reqexts = NULL;
static char *opt_sans = NULL;
static int opt_san_nodefault = 0;
static char *opt_policies = NULL;
static char *opt_policy_oids = NULL;
static int opt_policy_oids_critical = 0;
static int opt_popo = OSSL_CRMF_POPO_NONE - 1;
static char *opt_csr = NULL;
static char *opt_out_trusted = NULL;
static int opt_implicit_confirm = 0;
static int opt_disable_confirm = 0;
static char *opt_certout = NULL;

/* certificate enrollment and revocation */
static char *opt_oldcert = NULL;
static int opt_revreason = CRL_REASON_NONE;

/* credentials format */
static char *opt_certform_s = "PEM";
static int opt_certform = FORMAT_PEM;
static char *opt_keyform_s = "PEM";
static int opt_keyform = FORMAT_PEM;
static char *opt_certsform_s = "PEM";
static int opt_certsform = FORMAT_PEM;
static char *opt_otherpass = NULL;
static char *opt_engine = NULL;

/* TLS connection */
static int opt_tls_used = 0;
static char *opt_tls_cert = NULL;
static char *opt_tls_key = NULL;
static char *opt_tls_keypass = NULL;
static char *opt_tls_extra = NULL;
static char *opt_tls_trusted = NULL;
static char *opt_tls_host = NULL;

/* client-side debugging */
static int opt_batch = 0;
static int opt_repeat = 1;
static char *opt_reqin = NULL;
static int opt_reqin_new_tid = 0;
static char *opt_reqout = NULL;
static char *opt_rspin = NULL;
static char *opt_rspout = NULL;
static int opt_use_mock_srv = 0;

/* server-side debugging */
static char *opt_port = NULL;
static int opt_max_msgs = 0;

static char *opt_srv_ref = NULL;
static char *opt_srv_secret = NULL;
static char *opt_srv_cert = NULL;
static char *opt_srv_key = NULL;
static char *opt_srv_keypass = NULL;

static char *opt_srv_trusted = NULL;
static char *opt_srv_untrusted = NULL;
static char *opt_rsp_cert = NULL;
static char *opt_rsp_extracerts = NULL;
static char *opt_rsp_capubs = NULL;
static int opt_poll_count = 0;
static int opt_check_after = 1;
static int opt_grant_implicitconf = 0;

static int opt_pkistatus = OSSL_CMP_PKISTATUS_accepted;
static int opt_failure = INT_MIN;
static int opt_failurebits = 0;
static char *opt_statusstring = NULL;
static int opt_send_error = 0;
static int opt_send_unprotected = 0;
static int opt_send_unprot_err = 0;
static int opt_accept_unprotected = 0;
static int opt_accept_unprot_err = 0;
static int opt_accept_raverified = 0;

static X509_VERIFY_PARAM *vpm = NULL;

typedef enum OPTION_choice {
    OPT_ERR = -1, OPT_EOF = 0, OPT_HELP,
    OPT_CONFIG, OPT_SECTION,

    OPT_CMD, OPT_INFOTYPE, OPT_GENINFO,

    OPT_NEWKEY, OPT_NEWKEYPASS, OPT_SUBJECT, OPT_ISSUER,
    OPT_DAYS, OPT_REQEXTS,
    OPT_SANS, OPT_SAN_NODEFAULT,
    OPT_POLICIES, OPT_POLICY_OIDS, OPT_POLICY_OIDS_CRITICAL,
    OPT_POPO, OPT_CSR,
    OPT_OUT_TRUSTED, OPT_IMPLICIT_CONFIRM, OPT_DISABLE_CONFIRM,
    OPT_CERTOUT,

    OPT_OLDCERT, OPT_REVREASON,

    OPT_SERVER, OPT_PROXY, OPT_NO_PROXY, OPT_PATH,
    OPT_MSG_TIMEOUT, OPT_TOTAL_TIMEOUT,

    OPT_TRUSTED, OPT_UNTRUSTED, OPT_SRVCERT,
    OPT_RECIPIENT, OPT_EXPECT_SENDER,
    OPT_IGNORE_KEYUSAGE, OPT_UNPROTECTED_ERRORS,
    OPT_EXTRACERTSOUT, OPT_CACERTSOUT,

    OPT_REF, OPT_SECRET, OPT_CERT, OPT_KEY, OPT_KEYPASS,
    OPT_DIGEST, OPT_MAC, OPT_EXTRACERTS,
    OPT_UNPROTECTED_REQUESTS,

    OPT_CERTFORM, OPT_KEYFORM, OPT_CERTSFORM,
    OPT_OTHERPASS,
#ifndef OPENSSL_NO_ENGINE
    OPT_ENGINE,
#endif
    OPT_PROV_ENUM,

    OPT_TLS_USED, OPT_TLS_CERT, OPT_TLS_KEY,
    OPT_TLS_KEYPASS,
    OPT_TLS_EXTRA, OPT_TLS_TRUSTED, OPT_TLS_HOST,

    OPT_BATCH, OPT_REPEAT,
    OPT_REQIN, OPT_REQIN_NEW_TID, OPT_REQOUT, OPT_RSPIN, OPT_RSPOUT,

    OPT_USE_MOCK_SRV, OPT_PORT, OPT_MAX_MSGS,
    OPT_SRV_REF, OPT_SRV_SECRET,
    OPT_SRV_CERT, OPT_SRV_KEY, OPT_SRV_KEYPASS,
    OPT_SRV_TRUSTED, OPT_SRV_UNTRUSTED,
    OPT_RSP_CERT, OPT_RSP_EXTRACERTS, OPT_RSP_CAPUBS,
    OPT_POLL_COUNT, OPT_CHECK_AFTER,
    OPT_GRANT_IMPLICITCONF,
    OPT_PKISTATUS, OPT_FAILURE,
    OPT_FAILUREBITS, OPT_STATUSSTRING,
    OPT_SEND_ERROR, OPT_SEND_UNPROTECTED,
    OPT_SEND_UNPROT_ERR, OPT_ACCEPT_UNPROTECTED,
    OPT_ACCEPT_UNPROT_ERR, OPT_ACCEPT_RAVERIFIED,

    OPT_V_ENUM
} OPTION_CHOICE;

const OPTIONS cmp_options[] = {
    /* entries must be in the same order as enumerated above!! */
    {"help", OPT_HELP, '-', "Display this summary"},
    {"config", OPT_CONFIG, 's',
     "Configuration file to use. \"\" = none. Default from env variable OPENSSL_CONF"},
    {"section", OPT_SECTION, 's',
     "Section(s) in config file to get options from. \"\" = 'default'. Default 'cmp'"},

    OPT_SECTION("Generic message"),
    {"cmd", OPT_CMD, 's', "CMP request to send: ir/cr/kur/p10cr/rr/genm"},
    {"infotype", OPT_INFOTYPE, 's',
     "InfoType name for requesting specific info in genm, e.g. 'signKeyPairTypes'"},
    {"geninfo", OPT_GENINFO, 's',
     "generalInfo integer values to place in request PKIHeader with given OID"},
    {OPT_MORE_STR, 0, 0,
     "specified in the form <OID>:int:<n>, e.g. \"1.2.3:int:987\""},

    OPT_SECTION("Certificate enrollment"),
    {"newkey", OPT_NEWKEY, 's',
     "Private or public key for the requested cert. Default: CSR key or client key"},
    {"newkeypass", OPT_NEWKEYPASS, 's', "New private key pass phrase source"},
    {"subject", OPT_SUBJECT, 's',
     "Distinguished Name (DN) of subject to use in the requested cert template"},
    {OPT_MORE_STR, 0, 0,
     "For kur, default is the subject DN of the reference cert (see -oldcert);"},
    {OPT_MORE_STR, 0, 0,
     "this default is used for ir and cr only if no Subject Alt Names are set"},
    {"issuer", OPT_ISSUER, 's',
     "DN of the issuer to place in the requested certificate template"},
    {OPT_MORE_STR, 0, 0,
     "also used as recipient if neither -recipient nor -srvcert are given"},
    {"days", OPT_DAYS, 'n',
     "Requested validity time of the new certificate in number of days"},
    {"reqexts", OPT_REQEXTS, 's',
     "Name of config file section defining certificate request extensions"},
    {"sans", OPT_SANS, 's',
     "Subject Alt Names (IPADDR/DNS/URI) to add as (critical) cert req extension"},
    {"san_nodefault", OPT_SAN_NODEFAULT, '-',
     "Do not take default SANs from reference certificate (see -oldcert)"},
    {"policies", OPT_POLICIES, 's',
     "Name of config file section defining policies certificate request extension"},
    {"policy_oids", OPT_POLICY_OIDS, 's',
     "Policy OID(s) to add as policies certificate request extension"},
    {"policy_oids_critical", OPT_POLICY_OIDS_CRITICAL, '-',
     "Flag the policy OID(s) given with -policy_oids as critical"},
    {"popo", OPT_POPO, 'n',
     "Proof-of-Possession (POPO) method to use for ir/cr/kur where"},
    {OPT_MORE_STR, 0, 0,
     "-1 = NONE, 0 = RAVERIFIED, 1 = SIGNATURE (default), 2 = KEYENC"},
    {"csr", OPT_CSR, 's',
     "CSR file in PKCS#10 format to use in p10cr for legacy support"},
    {"out_trusted", OPT_OUT_TRUSTED, 's',
     "Certificates to trust when verifying newly enrolled certificates"},
    {"implicit_confirm", OPT_IMPLICIT_CONFIRM, '-',
     "Request implicit confirmation of newly enrolled certificates"},
    {"disable_confirm", OPT_DISABLE_CONFIRM, '-',
     "Do not confirm newly enrolled certificate w/o requesting implicit"},
    {OPT_MORE_STR, 0, 0,
     "confirmation. WARNING: This leads to behavior violating RFC 4210"},
    {"certout", OPT_CERTOUT, 's',
     "File to save newly enrolled certificate"},

    OPT_SECTION("Certificate enrollment and revocation"),

    {"oldcert", OPT_OLDCERT, 's',
     "Certificate to be updated (defaulting to -cert) or to be revoked in rr;"},
    {OPT_MORE_STR, 0, 0,
     "also used as reference (defaulting to -cert) for subject DN and SANs."},
    {OPT_MORE_STR, 0, 0,
     "Its issuer is used as recipient unless -srvcert, -recipient or -issuer given"},
    {"revreason", OPT_REVREASON, 'n',
     "Reason code to include in revocation request (rr); possible values:"},
    {OPT_MORE_STR, 0, 0,
     "0..6, 8..10 (see RFC5280, 5.3.1) or -1. Default -1 = none included"},

    OPT_SECTION("Message transfer"),
    {"server", OPT_SERVER, 's',
     "[http[s]://]address[:port] of CMP server. Default port 80 or 443."},
    {OPT_MORE_STR, 0, 0,
     "The address may be a DNS name or an IP address"},
    {"proxy", OPT_PROXY, 's',
     "[http[s]://]address[:port][/path] of HTTP(S) proxy to use; path is ignored"},
    {"no_proxy", OPT_NO_PROXY, 's',
     "List of addresses of servers not to use HTTP(S) proxy for"},
    {OPT_MORE_STR, 0, 0,
     "Default from environment variable 'no_proxy', else 'NO_PROXY', else none"},
    {"path", OPT_PATH, 's',
     "HTTP path (aka CMP alias) at the CMP server. Default \"/\""},
    {"msg_timeout", OPT_MSG_TIMEOUT, 'n',
     "Timeout per CMP message round trip (or 0 for none). Default 120 seconds"},
    {"total_timeout", OPT_TOTAL_TIMEOUT, 'n',
     "Overall time an enrollment incl. polling may take. Default 0 = infinite"},

    OPT_SECTION("Server authentication"),
    {"trusted", OPT_TRUSTED, 's',
     "Certificates to trust as chain roots when verifying signed CMP responses"},
    {OPT_MORE_STR, 0, 0, "unless -srvcert is given"},
    {"untrusted", OPT_UNTRUSTED, 's',
     "Intermediate certs for chain construction verifying CMP/TLS/enrolled certs"},
    {"srvcert", OPT_SRVCERT, 's',
     "Server cert to pin and trust directly when verifying signed CMP responses"},
    {"recipient", OPT_RECIPIENT, 's',
     "Distinguished Name (DN) to use as msg recipient; see man page for defaults"},
    {"expect_sender", OPT_EXPECT_SENDER, 's',
     "DN of expected sender of responses. Defaults to subject of -srvcert, if any"},
    {"ignore_keyusage", OPT_IGNORE_KEYUSAGE, '-',
     "Ignore CMP signer cert key usage, else 'digitalSignature' must be allowed"},
    {"unprotected_errors", OPT_UNPROTECTED_ERRORS, '-',
     "Accept missing or invalid protection of regular error messages and negative"},
    {OPT_MORE_STR, 0, 0,
     "certificate responses (ip/cp/kup), revocation responses (rp), and PKIConf"},
    {OPT_MORE_STR, 0, 0,
     "WARNING: This setting leads to behavior allowing violation of RFC 4210"},
    {"extracertsout", OPT_EXTRACERTSOUT, 's',
     "File to save extra certificates received in the extraCerts field"},
    {"cacertsout", OPT_CACERTSOUT, 's',
     "File to save CA certificates received in the caPubs field of 'ip' messages"},

    OPT_SECTION("Client authentication"),
    {"ref", OPT_REF, 's',
     "Reference value to use as senderKID in case no -cert is given"},
    {"secret", OPT_SECRET, 's',
     "Password source for client authentication with a pre-shared key (secret)"},
    {"cert", OPT_CERT, 's',
     "Client's current certificate (needed unless using -secret for PBM);"},
    {OPT_MORE_STR, 0, 0,
     "any further certs included are appended in extraCerts field"},
    {"key", OPT_KEY, 's', "Private key for the client's current certificate"},
    {"keypass", OPT_KEYPASS, 's',
     "Client private key (and cert and old cert file) pass phrase source"},
    {"digest", OPT_DIGEST, 's',
     "Digest to use in message protection and POPO signatures. Default \"sha256\""},
    {"mac", OPT_MAC, 's',
     "MAC algorithm to use in PBM-based message protection. Default \"hmac-sha1\""},
    {"extracerts", OPT_EXTRACERTS, 's',
     "Certificates to append in extraCerts field of outgoing messages"},
    {"unprotected_requests", OPT_UNPROTECTED_REQUESTS, '-',
     "Send messages without CMP-level protection"},

    OPT_SECTION("Credentials format"),
    {"certform", OPT_CERTFORM, 's',
     "Format (PEM or DER) to use when saving a certificate to a file. Default PEM"},
    {OPT_MORE_STR, 0, 0,
     "This also determines format to use for writing (not supported for P12)"},
    {"keyform", OPT_KEYFORM, 's',
     "Format to assume when reading key files. Default PEM"},
    {"certsform", OPT_CERTSFORM, 's',
     "Format (PEM/DER/P12) to try first reading multiple certs. Default PEM"},
    {"otherpass", OPT_OTHERPASS, 's',
     "Pass phrase source potentially needed for loading certificates of others"},
#ifndef OPENSSL_NO_ENGINE
    {"engine", OPT_ENGINE, 's',
     "Use crypto engine with given identifier, possibly a hardware device."},
    {OPT_MORE_STR, 0, 0,
     "Engines may be defined in OpenSSL config file engine section."},
    {OPT_MORE_STR, 0, 0,
     "Options like -key specifying keys held in the engine can give key IDs"},
    {OPT_MORE_STR, 0, 0,
     "prefixed by 'engine:', e.g. '-key engine:pkcs11:object=mykey;pin-value=1234'"},
#endif
    OPT_PROV_OPTIONS,

    OPT_SECTION("TLS connection"),
    {"tls_used", OPT_TLS_USED, '-',
     "Enable using TLS (also when other TLS options are not set)"},
    {"tls_cert", OPT_TLS_CERT, 's',
     "Client's TLS certificate. May include chain to be provided to TLS server"},
    {"tls_key", OPT_TLS_KEY, 's',
     "Private key for the client's TLS certificate"},
    {"tls_keypass", OPT_TLS_KEYPASS, 's',
     "Pass phrase source for the client's private TLS key (and TLS cert file)"},
    {"tls_extra", OPT_TLS_EXTRA, 's',
     "Extra certificates to provide to TLS server during TLS handshake"},
    {"tls_trusted", OPT_TLS_TRUSTED, 's',
     "Trusted certificates to use for verifying the TLS server certificate;"},
    {OPT_MORE_STR, 0, 0, "this implies host name validation"},
    {"tls_host", OPT_TLS_HOST, 's',
     "Address to be checked (rather than -server) during TLS host name validation"},

    OPT_SECTION("Client-side debugging"),
    {"batch", OPT_BATCH, '-',
     "Do not interactively prompt for input when a password is required etc."},
    {"repeat", OPT_REPEAT, 'n',
     "Invoke the transaction the given number of times. Default 1"},
    {"reqin", OPT_REQIN, 's', "Take sequence of CMP requests from file(s)"},
    {"reqin_new_tid", OPT_REQIN_NEW_TID, '-',
     "Use fresh transactionID for CMP requests read from -reqin"},
    {"reqout", OPT_REQOUT, 's', "Save sequence of CMP requests to file(s)"},
    {"rspin", OPT_RSPIN, 's',
     "Process sequence of CMP responses provided in file(s), skipping server"},
    {"rspout", OPT_RSPOUT, 's', "Save sequence of CMP responses to file(s)"},

    {"use_mock_srv", OPT_USE_MOCK_SRV, '-', "Use mock server at API level, bypassing HTTP"},

    OPT_SECTION("Mock server"),
    {"port", OPT_PORT, 's', "Act as HTTP mock server listening on given port"},
    {"max_msgs", OPT_MAX_MSGS, 'n',
     "max number of messages handled by HTTP mock server. Default: 0 = unlimited"},

    {"srv_ref", OPT_SRV_REF, 's',
     "Reference value to use as senderKID of server in case no -srv_cert is given"},
    {"srv_secret", OPT_SRV_SECRET, 's',
     "Password source for server authentication with a pre-shared key (secret)"},
    {"srv_cert", OPT_SRV_CERT, 's', "Certificate of the server"},
    {"srv_key", OPT_SRV_KEY, 's',
     "Private key used by the server for signing messages"},
    {"srv_keypass", OPT_SRV_KEYPASS, 's',
     "Server private key (and cert) file pass phrase source"},

    {"srv_trusted", OPT_SRV_TRUSTED, 's',
     "Trusted certificates for client authentication"},
    {"srv_untrusted", OPT_SRV_UNTRUSTED, 's',
     "Intermediate certs that may be useful for verifying CMP protection"},
    {"rsp_cert", OPT_RSP_CERT, 's',
     "Certificate to be returned as mock enrollment result"},
    {"rsp_extracerts", OPT_RSP_EXTRACERTS, 's',
     "Extra certificates to be included in mock certification responses"},
    {"rsp_capubs", OPT_RSP_CAPUBS, 's',
     "CA certificates to be included in mock ip response"},
    {"poll_count", OPT_POLL_COUNT, 'n',
     "Number of times the client must poll before receiving a certificate"},
    {"check_after", OPT_CHECK_AFTER, 'n',
     "The check_after value (time to wait) to include in poll response"},
    {"grant_implicitconf", OPT_GRANT_IMPLICITCONF, '-',
     "Grant implicit confirmation of newly enrolled certificate"},

    {"pkistatus", OPT_PKISTATUS, 'n',
     "PKIStatus to be included in server response. Possible values: 0..6"},
    {"failure", OPT_FAILURE, 'n',
     "A single failure info bit number to include in server response, 0..26"},
    {"failurebits", OPT_FAILUREBITS, 'n',
     "Number representing failure bits to include in server response, 0..2^27 - 1"},
    {"statusstring", OPT_STATUSSTRING, 's',
     "Status string to be included in server response"},
    {"send_error", OPT_SEND_ERROR, '-',
     "Force server to reply with error message"},
    {"send_unprotected", OPT_SEND_UNPROTECTED, '-',
     "Send response messages without CMP-level protection"},
    {"send_unprot_err", OPT_SEND_UNPROT_ERR, '-',
     "In case of negative responses, server shall send unprotected error messages,"},
    {OPT_MORE_STR, 0, 0,
     "certificate responses (ip/cp/kup), and revocation responses (rp)."},
    {OPT_MORE_STR, 0, 0,
     "WARNING: This setting leads to behavior violating RFC 4210"},
    {"accept_unprotected", OPT_ACCEPT_UNPROTECTED, '-',
     "Accept missing or invalid protection of requests"},
    {"accept_unprot_err", OPT_ACCEPT_UNPROT_ERR, '-',
     "Accept unprotected error messages from client"},
    {"accept_raverified", OPT_ACCEPT_RAVERIFIED, '-',
     "Accept RAVERIFIED as proof-of-possession (POPO)"},

    OPT_V_OPTIONS,
    {NULL}
};

typedef union {
    char **txt;
    int *num;
    long *num_long;
} varref;
static varref cmp_vars[] = { /* must be in same order as enumerated above! */
    {&opt_config}, {&opt_section},

    {&opt_cmd_s}, {&opt_infotype_s}, {&opt_geninfo},

    {&opt_newkey}, {&opt_newkeypass}, {&opt_subject}, {&opt_issuer},
    {(char **)&opt_days}, {&opt_reqexts},
    {&opt_sans}, {(char **)&opt_san_nodefault},
    {&opt_policies}, {&opt_policy_oids}, {(char **)&opt_policy_oids_critical},
    {(char **)&opt_popo}, {&opt_csr},
    {&opt_out_trusted},
    {(char **)&opt_implicit_confirm}, {(char **)&opt_disable_confirm},
    {&opt_certout},

    {&opt_oldcert}, {(char **)&opt_revreason},

    {&opt_server}, {&opt_proxy}, {&opt_no_proxy}, {&opt_path},
    {(char **)&opt_msg_timeout}, {(char **)&opt_total_timeout},

    {&opt_trusted}, {&opt_untrusted}, {&opt_srvcert},
    {&opt_recipient}, {&opt_expect_sender},
    {(char **)&opt_ignore_keyusage}, {(char **)&opt_unprotected_errors},
    {&opt_extracertsout}, {&opt_cacertsout},

    {&opt_ref}, {&opt_secret}, {&opt_cert}, {&opt_key}, {&opt_keypass},
    {&opt_digest}, {&opt_mac}, {&opt_extracerts},
    {(char **)&opt_unprotected_requests},

    {&opt_certform_s}, {&opt_keyform_s}, {&opt_certsform_s},
    {&opt_otherpass},
#ifndef OPENSSL_NO_ENGINE
    {&opt_engine},
#endif

    {(char **)&opt_tls_used}, {&opt_tls_cert}, {&opt_tls_key},
    {&opt_tls_keypass},
    {&opt_tls_extra}, {&opt_tls_trusted}, {&opt_tls_host},

    {(char **)&opt_batch}, {(char **)&opt_repeat},
    {&opt_reqin}, {(char **)&opt_reqin_new_tid},
    {&opt_reqout}, {&opt_rspin}, {&opt_rspout},

    {(char **)&opt_use_mock_srv}, {&opt_port}, {(char **)&opt_max_msgs},
    {&opt_srv_ref}, {&opt_srv_secret},
    {&opt_srv_cert}, {&opt_srv_key}, {&opt_srv_keypass},
    {&opt_srv_trusted}, {&opt_srv_untrusted},
    {&opt_rsp_cert}, {&opt_rsp_extracerts}, {&opt_rsp_capubs},
    {(char **)&opt_poll_count}, {(char **)&opt_check_after},
    {(char **)&opt_grant_implicitconf},
    {(char **)&opt_pkistatus}, {(char **)&opt_failure},
    {(char **)&opt_failurebits}, {&opt_statusstring},
    {(char **)&opt_send_error}, {(char **)&opt_send_unprotected},
    {(char **)&opt_send_unprot_err}, {(char **)&opt_accept_unprotected},
    {(char **)&opt_accept_unprot_err}, {(char **)&opt_accept_raverified},

    {NULL}
};

#ifndef NDEBUG
# define FUNC (strcmp(OPENSSL_FUNC, "(unknown function)") == 0  \
               ? "CMP" : "OPENSSL_FUNC")
# define PRINT_LOCATION(bio) BIO_printf(bio, "%s:%s:%d:", \
                                        FUNC, OPENSSL_FILE, OPENSSL_LINE)
#else
# define PRINT_LOCATION(bio) ((void)0)
#endif
#define CMP_print(bio, prefix, msg, a1, a2, a3) \
    (PRINT_LOCATION(bio), \
     BIO_printf(bio, "CMP %s: " msg "\n", prefix, a1, a2, a3))
#define CMP_INFO(msg, a1, a2, a3)  CMP_print(bio_out, "info", msg, a1, a2, a3)
#define CMP_info(msg)              CMP_INFO(msg"%s%s%s", "", "", "")
#define CMP_info1(msg, a1)         CMP_INFO(msg"%s%s",   a1, "", "")
#define CMP_info2(msg, a1, a2)     CMP_INFO(msg"%s",     a1, a2, "")
#define CMP_info3(msg, a1, a2, a3) CMP_INFO(msg,         a1, a2, a3)
#define CMP_WARN(m, a1, a2, a3)    CMP_print(bio_out, "warning", m, a1, a2, a3)
#define CMP_warn(msg)              CMP_WARN(msg"%s%s%s", "", "", "")
#define CMP_warn1(msg, a1)         CMP_WARN(msg"%s%s",   a1, "", "")
#define CMP_warn2(msg, a1, a2)     CMP_WARN(msg"%s",     a1, a2, "")
#define CMP_warn3(msg, a1, a2, a3) CMP_WARN(msg,         a1, a2, a3)
#define CMP_ERR(msg, a1, a2, a3)   CMP_print(bio_err, "error", msg, a1, a2, a3)
#define CMP_err(msg)               CMP_ERR(msg"%s%s%s", "", "", "")
#define CMP_err1(msg, a1)          CMP_ERR(msg"%s%s",   a1, "", "")
#define CMP_err2(msg, a1, a2)      CMP_ERR(msg"%s",     a1, a2, "")
#define CMP_err3(msg, a1, a2, a3)  CMP_ERR(msg,         a1, a2, a3)

static int print_to_bio_out(const char *func, const char *file, int line,
                            OSSL_CMP_severity level, const char *msg)
{
    return OSSL_CMP_print_to_bio(bio_out, func, file, line, level, msg);
}

/* code duplicated from crypto/cmp/cmp_util.c */
static int sk_X509_add1_cert(STACK_OF(X509) *sk, X509 *cert,
                             int no_dup, int prepend)
{
    if (no_dup) {
        /*
         * not using sk_X509_set_cmp_func() and sk_X509_find()
         * because this re-orders the certs on the stack
         */
        int i;

        for (i = 0; i < sk_X509_num(sk); i++) {
            if (X509_cmp(sk_X509_value(sk, i), cert) == 0)
                return 1;
        }
    }
    if (!X509_up_ref(cert))
        return 0;
    if (!sk_X509_insert(sk, cert, prepend ? 0 : -1)) {
        X509_free(cert);
        return 0;
    }
    return 1;
}

/* code duplicated from crypto/cmp/cmp_util.c */
static int sk_X509_add1_certs(STACK_OF(X509) *sk, STACK_OF(X509) *certs,
                              int no_self_signed, int no_dups, int prepend)
/* compiler would allow 'const' for the list of certs, yet they are up-ref'ed */
{
    int i;

    if (sk == NULL)
        return 0;
    if (certs == NULL)
        return 1;
    for (i = 0; i < sk_X509_num(certs); i++) {
        X509 *cert = sk_X509_value(certs, i);

        if (!no_self_signed || X509_check_issued(cert, cert) != X509_V_OK) {
            if (!sk_X509_add1_cert(sk, cert, no_dups, prepend))
                return 0;
        }
    }
    return 1;
}

/* TODO potentially move to apps/lib/apps.c */
static char *next_item(char *opt) /* in list separated by comma and/or space */
{
    /* advance to separator (comma or whitespace), if any */
    while (*opt != ',' && !isspace(*opt) && *opt != '\0') {
        if (*opt == '\\' && opt[1] != '\0')
            /* skip and unescape '\' escaped char */
            memmove(opt, opt + 1, strlen(opt));
        opt++;
    }
    if (*opt != '\0') {
        /* terminate current item */
        *opt++ = '\0';
        /* skip over any whitespace after separator */
        while (isspace(*opt))
            opt++;
    }
    return *opt == '\0' ? NULL : opt; /* NULL indicates end of input */
}

static EVP_PKEY *load_key_pwd(const char *uri, int format,
                              const char *pass, ENGINE *e, const char *desc)
{
    char *pass_string = get_passwd(pass, desc);
    EVP_PKEY *pkey = load_key(uri, format, 0, pass_string, e, desc);

    clear_free(pass_string);
    return pkey;
}

static X509 *load_cert_pwd(const char *uri, const char *pass, const char *desc)
{
    X509 *cert;
    char *pass_string = get_passwd(pass, desc);

    cert = load_cert_pass(uri, 0, pass_string, desc);
    clear_free(pass_string);
    return cert;
}

/* TODO remove when PR #4930 is merged */
static int load_pkcs12(BIO *in, const char *desc,
                       pem_password_cb *pem_cb, void *cb_data,
                       EVP_PKEY **pkey, X509 **cert, STACK_OF(X509) **ca)
{
    const char *pass;
    char tpass[PEM_BUFSIZE];
    int len;
    int ret = 0;
    PKCS12 *p12 = d2i_PKCS12_bio(in, NULL);

    if (desc == NULL)
        desc = "PKCS12 input";
    if (p12 == NULL) {
        BIO_printf(bio_err, "error loading PKCS12 file for %s\n", desc);
        goto die;
    }

    /* See if an empty password will do */
    if (PKCS12_verify_mac(p12, "", 0) || PKCS12_verify_mac(p12, NULL, 0)) {
        pass = "";
    } else {
        if (pem_cb == NULL)
            pem_cb = wrap_password_callback;
        len = pem_cb(tpass, PEM_BUFSIZE, 0, cb_data);
        if (len < 0) {
            BIO_printf(bio_err, "passphrase callback error for %s\n", desc);
            goto die;
        }
        if (len < PEM_BUFSIZE)
            tpass[len] = 0;
        if (!PKCS12_verify_mac(p12, tpass, len)) {
            BIO_printf(bio_err,
                       "mac verify error (wrong password?) in PKCS12 file for %s\n",
                       desc);
            goto die;
        }
        pass = tpass;
    }
    ret = PKCS12_parse(p12, pass, pkey, cert, ca);
 die:
    PKCS12_free(p12);
    return ret;
}

/* TODO potentially move this and related functions to apps/lib/apps.c */
static int adjust_format(const char **infile, int format, int engine_ok)
{
    if (!strncasecmp(*infile, "http://", 7)
            || !strncasecmp(*infile, "https://", 8)) {
        format = FORMAT_HTTP;
    } else if (engine_ok && strncasecmp(*infile, "engine:", 7) == 0) {
        *infile += 7;
        format = FORMAT_ENGINE;
    } else {
        if (strncasecmp(*infile, "file:", 5) == 0)
            *infile += 5;
        /*
         * the following is a heuristic whether first to try PEM or DER
         * or PKCS12 as the input format for files
         */
        if (strlen(*infile) >= 4) {
            const char *extension = *infile + strlen(*infile) - 4;

            if (strncasecmp(extension, ".crt", 4) == 0
                    || strncasecmp(extension, ".pem", 4) == 0)
                /* weak recognition of PEM format */
                format = FORMAT_PEM;
            else if (strncasecmp(extension, ".cer", 4) == 0
                         || strncasecmp(extension, ".der", 4) == 0)
                /* weak recognition of DER format */
                format = FORMAT_ASN1;
            else if (strncasecmp(extension, ".p12", 4) == 0)
                /* weak recognition of PKCS#12 format */
                format = FORMAT_PKCS12;
            /* else retain given format */
        }
    }
    return format;
}

/*
 * TODO potentially move this and related functions to apps/lib/
 * or even better extend OSSL_STORE with type OSSL_STORE_INFO_CRL
 */
static X509_REQ *load_csr_autofmt(const char *infile, const char *desc)
{
    X509_REQ *csr;
    BIO *bio_bak = bio_err;
    int can_retry;
    int format = adjust_format(&infile, FORMAT_PEM, 0);

    can_retry = format == FORMAT_PEM || format == FORMAT_ASN1;
    if (can_retry)
        bio_err = NULL; /* do not show errors on more than one try */
    csr = load_csr(infile, format, desc);
    bio_err = bio_bak;
    if (csr == NULL && can_retry) {
        ERR_clear_error();
        format = (format == FORMAT_PEM ? FORMAT_ASN1 : FORMAT_PEM);
        csr = load_csr(infile, format, desc);
    }
    if (csr == NULL) {
        ERR_print_errors(bio_err);
        BIO_printf(bio_err, "error: unable to load %s from file '%s'\n", desc,
                   infile);
    }
    return csr;
}

/* TODO replace by calling generalized load_certs() when PR #4930 is merged */
static int load_certs_preliminary(const char *file, STACK_OF(X509) **certs,
                                  int format, const char *pass,
                                  const char *desc)
{
    X509 *cert = NULL;
    int ret = 0;

    if (format == FORMAT_PKCS12) {
        BIO *bio = bio_open_default(file, 'r', format);

        if (bio != NULL) {
            EVP_PKEY *pkey = NULL; /* pkey is needed until PR #4930 is merged */
            PW_CB_DATA cb_data;

            cb_data.password = pass;
            cb_data.prompt_info = file;
            ret = load_pkcs12(bio, desc, wrap_password_callback,
                              &cb_data, &pkey, &cert, certs);
            EVP_PKEY_free(pkey);
            BIO_free(bio);
        }
    } else if (format == FORMAT_ASN1) { /* load only one cert in this case */
        CMP_warn1("can load only one certificate in DER format from %s", file);
        cert = load_cert_pass(file, 0, pass, desc);
    }
    if (format == FORMAT_PKCS12 || format == FORMAT_ASN1) {
        if (cert) {
            if (*certs == NULL)
                *certs = sk_X509_new_null();
            if (*certs != NULL)
                ret = sk_X509_insert(*certs, cert, 0);
            else
                X509_free(cert);
        }
    } else {
        ret = load_certs(file, certs, format, pass, desc);
    }
    return ret;
}

static void warn_certs_expired(const char *file, STACK_OF(X509) **certs)
{
    int i, res;
    X509 *cert;
    char *subj;

    for (i = 0; i < sk_X509_num(*certs); i++) {
        cert = sk_X509_value(*certs, i);
        res = X509_cmp_timeframe(vpm, X509_get0_notBefore(cert),
                                 X509_get0_notAfter(cert));
        if (res != 0) {
            subj = X509_NAME_oneline(X509_get_subject_name(cert), NULL, 0);
            CMP_warn3("certificate from '%s' with subject '%s' %s", file, subj,
                      res > 0 ? "has expired" : "not yet valid");
            OPENSSL_free(subj);
        }
    }
}

/*
 * TODO potentially move this and related functions to apps/lib/
 * or even better extend OSSL_STORE with type OSSL_STORE_INFO_CERTS
 */
static int load_certs_autofmt(const char *infile, STACK_OF(X509) **certs,
                              int exclude_http, const char *pass,
                              const char *desc)
{
    int ret = 0;
    char *pass_string;
    BIO *bio_bak = bio_err;
    int format = adjust_format(&infile, opt_certsform, 0);

    if (exclude_http && format == FORMAT_HTTP) {
        BIO_printf(bio_err, "error: HTTP retrieval not allowed for %s\n", desc);
        return ret;
    }
    pass_string = get_passwd(pass, desc);
    if (format != FORMAT_HTTP)
        bio_err = NULL; /* do not show errors on more than one try */
    ret = load_certs_preliminary(infile, certs, format, pass_string, desc);
    bio_err = bio_bak;
    if (!ret && format != FORMAT_HTTP) {
        int format2 = format == FORMAT_PEM ? FORMAT_ASN1 : FORMAT_PEM;

        ERR_clear_error();
        ret = load_certs_preliminary(infile, certs, format2, pass_string, desc);
    }
    clear_free(pass_string);

    if (ret)
        warn_certs_expired(infile, certs);
    return ret;
}

/* set expected host name/IP addr and clears the email addr in the given ts */
static int truststore_set_host_etc(X509_STORE *ts, char *host)
{
    X509_VERIFY_PARAM *ts_vpm = X509_STORE_get0_param(ts);

    /* first clear any host names, IP, and email addresses */
    if (!X509_VERIFY_PARAM_set1_host(ts_vpm, NULL, 0)
            || !X509_VERIFY_PARAM_set1_ip(ts_vpm, NULL, 0)
            || !X509_VERIFY_PARAM_set1_email(ts_vpm, NULL, 0))
        return 0;
    X509_VERIFY_PARAM_set_hostflags(ts_vpm,
                                    X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT |
                                    X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return (host != NULL && X509_VERIFY_PARAM_set1_ip_asc(ts_vpm, host))
        || X509_VERIFY_PARAM_set1_host(ts_vpm, host, 0);
}

static X509_STORE *sk_X509_to_store(X509_STORE *store /* may be NULL */,
                                    const STACK_OF(X509) *certs /* may NULL */)
{
    int i;

    if (store == NULL)
        store = X509_STORE_new();
    if (store == NULL)
        return NULL;
    for (i = 0; i < sk_X509_num(certs); i++) {
        if (!X509_STORE_add_cert(store, sk_X509_value(certs, i))) {
            X509_STORE_free(store);
            return NULL;
        }
    }
    return store;
}

/* write OSSL_CMP_MSG DER-encoded to the specified file name item */
static int write_PKIMESSAGE(const OSSL_CMP_MSG *msg, char **filenames)
{
    char *file;
    BIO *bio;

    if (msg == NULL || filenames == NULL) {
        CMP_err("NULL arg to write_PKIMESSAGE");
        return 0;
    }
    if (*filenames == NULL) {
        CMP_err("Not enough file names provided for writing PKIMessage");
        return 0;
    }

    file = *filenames;
    *filenames = next_item(file);
    bio = BIO_new_file(file, "wb");
    if (bio == NULL) {
        CMP_err1("Cannot open file '%s' for writing", file);
        return 0;
    }
    if (i2d_OSSL_CMP_MSG_bio(bio, msg) < 0) {
        CMP_err1("Cannot write PKIMessage to file '%s'", file);
        BIO_free(bio);
        return 0;
    }
    BIO_free(bio);
    return 1;
}

/* read DER-encoded OSSL_CMP_MSG from the specified file name item */
static OSSL_CMP_MSG *read_PKIMESSAGE(char **filenames)
{
    char *file;
    BIO *bio;
    OSSL_CMP_MSG *ret;

    if (filenames == NULL) {
        CMP_err("NULL arg to read_PKIMESSAGE");
        return NULL;
    }
    if (*filenames == NULL) {
        CMP_err("Not enough file names provided for reading PKIMessage");
        return NULL;
    }

    file = *filenames;
    *filenames = next_item(file);
    bio = BIO_new_file(file, "rb");
    if (bio == NULL) {
        CMP_err1("Cannot open file '%s' for reading", file);
        return NULL;
    }
    ret = d2i_OSSL_CMP_MSG_bio(bio, NULL);
    if (ret == NULL)
        CMP_err1("Cannot read PKIMessage from file '%s'", file);
    BIO_free(bio);
    return ret;
}

/*-
 * Sends the PKIMessage req and on success place the response in *res
 * basically like OSSL_CMP_MSG_http_perform(), but in addition allows
 * to dump the sequence of requests and responses to files and/or
 * to take the sequence of requests and responses from files.
 */
static OSSL_CMP_MSG *read_write_req_resp(OSSL_CMP_CTX *ctx,
                                         const OSSL_CMP_MSG *req)
{
    OSSL_CMP_MSG *req_new = NULL;
    OSSL_CMP_MSG *res = NULL;
    OSSL_CMP_PKIHEADER *hdr;

    if (req != NULL && opt_reqout != NULL
            && !write_PKIMESSAGE(req, &opt_reqout))
        goto err;
    if (opt_reqin != NULL && opt_rspin == NULL) {
        if ((req_new = read_PKIMESSAGE(&opt_reqin)) == NULL)
            goto err;
        /*-
         * The transaction ID in req_new read from opt_reqin may not be fresh.
         * In this case the server may complain "Transaction id already in use."
         * The following workaround unfortunately requires re-protection.
         */
        if (opt_reqin_new_tid
                && !OSSL_CMP_MSG_update_transactionID(ctx, req_new))
            goto err;
    }

    if (opt_rspin != NULL) {
        res = read_PKIMESSAGE(&opt_rspin);
    } else {
        const OSSL_CMP_MSG *actual_req = opt_reqin != NULL ? req_new : req;

        res = opt_use_mock_srv
            ? OSSL_CMP_CTX_server_perform(ctx, actual_req)
            : OSSL_CMP_MSG_http_perform(ctx, actual_req);
    }
    if (res == NULL)
        goto err;

    if (opt_reqin != NULL || opt_rspin != NULL) {
        /* need to satisfy nonce and transactionID checks */
        ASN1_OCTET_STRING *nonce;
        ASN1_OCTET_STRING *tid;

        hdr = OSSL_CMP_MSG_get0_header(res);
        nonce = OSSL_CMP_HDR_get0_recipNonce(hdr);
        tid = OSSL_CMP_HDR_get0_transactionID(hdr);
        if (!OSSL_CMP_CTX_set1_senderNonce(ctx, nonce)
                || !OSSL_CMP_CTX_set1_transactionID(ctx, tid)) {
            OSSL_CMP_MSG_free(res);
            res = NULL;
            goto err;
        }
    }

    if (opt_rspout != NULL && !write_PKIMESSAGE(res, &opt_rspout)) {
        OSSL_CMP_MSG_free(res);
        res = NULL;
    }

 err:
    OSSL_CMP_MSG_free(req_new);
    return res;
}

/*
 * parse string as integer value, not allowing trailing garbage, see also
 * https://www.gnu.org/software/libc/manual/html_node/Parsing-of-Integers.html
 *
 * returns integer value, or INT_MIN on error
 */
static int atoint(const char *str)
{
    char *tailptr;
    long res = strtol(str, &tailptr, 10);

    if  ((*tailptr != '\0') || (res < INT_MIN) || (res > INT_MAX))
        return INT_MIN;
    else
        return (int)res;
}

static int parse_addr(char **opt_string, int port, const char *name)
{
    char *port_string;

    if (strncasecmp(*opt_string, OSSL_HTTP_PREFIX,
                    strlen(OSSL_HTTP_PREFIX)) == 0) {
        *opt_string += strlen(OSSL_HTTP_PREFIX);
    } else if (strncasecmp(*opt_string, OSSL_HTTPS_PREFIX,
                           strlen(OSSL_HTTPS_PREFIX)) == 0) {
        *opt_string += strlen(OSSL_HTTPS_PREFIX);
        if (port == 0)
            port = 443; /* == integer value of OSSL_HTTPS_PORT */
    }

    if ((port_string = strrchr(*opt_string, ':')) == NULL)
        return port; /* using default */
    *(port_string++) = '\0';
    port = atoint(port_string);
    if ((port <= 0) || (port > 65535)) {
        CMP_err2("invalid %s port '%s' given, sane range 1-65535",
                 name, port_string);
        return -1;
    }
    return port;
}

static int set1_store_parameters(X509_STORE *ts)
{
    if (ts == NULL)
        return 0;

    /* copy vpm to store */
    if (!X509_STORE_set1_param(ts, vpm /* may be NULL */)) {
        BIO_printf(bio_err, "error setting verification parameters\n");
        OSSL_CMP_CTX_print_errors(cmp_ctx);
        return 0;
    }

    X509_STORE_set_verify_cb(ts, X509_STORE_CTX_print_verify_cb);

    return 1;
}

static int set_name(const char *str,
                    int (*set_fn) (OSSL_CMP_CTX *ctx, const X509_NAME *name),
                    OSSL_CMP_CTX *ctx, const char *desc)
{
    if (str != NULL) {
        X509_NAME *n = parse_name(str, MBSTRING_ASC, 0);

        if (n == NULL) {
            CMP_err2("cannot parse %s DN '%s'", desc, str);
            return 0;
        }
        if (!(*set_fn) (ctx, n)) {
            X509_NAME_free(n);
            CMP_err("out of memory");
            return 0;
        }
        X509_NAME_free(n);
    }
    return 1;
}

static int set_gennames(OSSL_CMP_CTX *ctx, char *names, const char *desc)
{
    char *next;

    for (; names != NULL; names = next) {
        GENERAL_NAME *n;

        next = next_item(names);
        if (strcmp(names, "critical") == 0) {
            (void)OSSL_CMP_CTX_set_option(ctx,
                                          OSSL_CMP_OPT_SUBJECTALTNAME_CRITICAL,
                                          1);
            continue;
        }

        /* try IP address first, then URI or domain name */
        (void)ERR_set_mark();
        n = a2i_GENERAL_NAME(NULL, NULL, NULL, GEN_IPADD, names, 0);
        if (n == NULL)
            n = a2i_GENERAL_NAME(NULL, NULL, NULL,
                                 strchr(names, ':') != NULL ? GEN_URI : GEN_DNS,
                                 names, 0);
        (void)ERR_pop_to_mark();

        if (n == NULL) {
            CMP_err2("bad syntax of %s '%s'", desc, names);
            return 0;
        }
        if (!OSSL_CMP_CTX_push1_subjectAltName(ctx, n)) {
            GENERAL_NAME_free(n);
            CMP_err("out of memory");
            return 0;
        }
        GENERAL_NAME_free(n);
    }
    return 1;
}

/* TODO potentially move to apps/lib/apps.c */
/*
 * create cert store structure with certificates read from given file(s)
 * returns pointer to created X509_STORE on success, NULL on error
 */
static X509_STORE *load_certstore(char *input, const char *desc)
{
    X509_STORE *store = NULL;
    STACK_OF(X509) *certs = NULL;

    if (input == NULL)
        goto err;

    while (input != NULL) {
        char *next = next_item(input);           \

        if (!load_certs_autofmt(input, &certs, 1, opt_otherpass, desc)
                || !(store = sk_X509_to_store(store, certs))) {
            /* CMP_err("out of memory"); */
            X509_STORE_free(store);
            store = NULL;
            goto err;
        }
        sk_X509_pop_free(certs, X509_free);
        certs = NULL;
        input = next;
    }
 err:
    sk_X509_pop_free(certs, X509_free);
    return store;
}

/* TODO potentially move to apps/lib/apps.c */
static STACK_OF(X509) *load_certs_multifile(char *files,
                                            const char *pass, const char *desc)
{
    STACK_OF(X509) *certs = NULL;
    STACK_OF(X509) *result = sk_X509_new_null();

    if (files == NULL)
        goto err;
    if (result == NULL)
        goto oom;

    while (files != NULL) {
        char *next = next_item(files);

        if (!load_certs_autofmt(files, &certs, 0, pass, desc))
            goto err;
        if (!sk_X509_add1_certs(result, certs, 0, 1 /* no dups */, 0))
            goto oom;
        sk_X509_pop_free(certs, X509_free);
        certs = NULL;
        files = next;
    }
    return result;

 oom:
    BIO_printf(bio_err, "out of memory\n");
 err:
    sk_X509_pop_free(certs, X509_free);
    sk_X509_pop_free(result, X509_free);
    return NULL;
}

typedef int (*add_X509_stack_fn_t)(void *ctx, const STACK_OF(X509) *certs);
typedef int (*add_X509_fn_t)(void *ctx, const X509 *cert);

static int setup_certs(char *files, const char *desc, void *ctx,
                       add_X509_stack_fn_t addn_fn, add_X509_fn_t add1_fn)
{
    int ret = 1;

    if (files != NULL) {
        STACK_OF(X509) *certs = load_certs_multifile(files, opt_otherpass,
                                                     desc);
        if (certs == NULL) {
            ret = 0;
        } else {
            if (addn_fn != NULL) {
                ret = (*addn_fn)(ctx, certs);
            } else {
                int i;

                for (i = 0; i < sk_X509_num(certs /* may be NULL */); i++)
                    ret &= (*add1_fn)(ctx, sk_X509_value(certs, i));
            }
            sk_X509_pop_free(certs, X509_free);
        }
    }
    return ret;
}


/*
 * parse and transform some options, checking their syntax.
 * Returns 1 on success, 0 on error
 */
static int transform_opts(void)
{
    if (opt_cmd_s != NULL) {
        if (!strcmp(opt_cmd_s, "ir")) {
            opt_cmd = CMP_IR;
        } else if (!strcmp(opt_cmd_s, "kur")) {
            opt_cmd = CMP_KUR;
        } else if (!strcmp(opt_cmd_s, "cr")) {
            opt_cmd = CMP_CR;
        } else if (!strcmp(opt_cmd_s, "p10cr")) {
            opt_cmd = CMP_P10CR;
        } else if (!strcmp(opt_cmd_s, "rr")) {
            opt_cmd = CMP_RR;
        } else if (!strcmp(opt_cmd_s, "genm")) {
            opt_cmd = CMP_GENM;
        } else {
            CMP_err1("unknown cmp command '%s'", opt_cmd_s);
            return 0;
        }
    } else {
        CMP_err("no cmp command to execute");
        return 0;
    }

#ifdef OPENSSL_NO_ENGINE
# define FORMAT_OPTIONS (OPT_FMT_PEMDER | OPT_FMT_PKCS12 | OPT_FMT_ENGINE)
#else
# define FORMAT_OPTIONS (OPT_FMT_PEMDER | OPT_FMT_PKCS12)
#endif

    if (opt_keyform_s != NULL
            && !opt_format(opt_keyform_s, FORMAT_OPTIONS, &opt_keyform)) {
        CMP_err("unknown option given for key loading format");
        return 0;
    }

#undef FORMAT_OPTIONS

    if (opt_certform_s != NULL
            && !opt_format(opt_certform_s, OPT_FMT_PEMDER, &opt_certform)) {
        CMP_err("unknown option given for certificate storing format");
        return 0;
    }

    if (opt_certsform_s != NULL
            && !opt_format(opt_certsform_s, OPT_FMT_PEMDER | OPT_FMT_PKCS12,
                           &opt_certsform)) {
        CMP_err("unknown option given for certificate list loading format");
        return 0;
    }

    return 1;
}

static OSSL_CMP_SRV_CTX *setup_srv_ctx(ENGINE *e)
{
    OSSL_CMP_CTX *ctx; /* extra CMP (client) ctx partly used by server */
    OSSL_CMP_SRV_CTX *srv_ctx = ossl_cmp_mock_srv_new();

    if (srv_ctx == NULL)
        return NULL;
    ctx = OSSL_CMP_SRV_CTX_get0_cmp_ctx(srv_ctx);

    if (opt_srv_ref == NULL) {
        if (opt_srv_cert == NULL) {
            /* opt_srv_cert should determine the sender */
            CMP_err("must give -srv_ref for server if no -srv_cert given");
            goto err;
        }
    } else {
        if (!OSSL_CMP_CTX_set1_referenceValue(ctx, (unsigned char *)opt_srv_ref,
                                              strlen(opt_srv_ref)))
            goto err;
    }

    if (opt_srv_secret != NULL) {
        int res;
        char *pass_str = get_passwd(opt_srv_secret, "PBMAC secret of server");

        if (pass_str != NULL) {
            cleanse(opt_srv_secret);
            res = OSSL_CMP_CTX_set1_secretValue(ctx, (unsigned char *)pass_str,
                                                strlen(pass_str));
            clear_free(pass_str);
            if (res == 0)
                goto err;
        }
    } else if (opt_srv_cert == NULL) {
        CMP_err("server credentials must be given if -use_mock_srv or -port is used");
        goto err;
    } else {
        CMP_warn("server will not be able to handle PBM-protected requests since -srv_secret is not given");
    }

    if (opt_srv_secret == NULL
            && ((opt_srv_cert == NULL) != (opt_srv_key == NULL))) {
        CMP_err("must give both -srv_cert and -srv_key options or neither");
        goto err;
    }
    if (opt_srv_cert != NULL) {
        X509 *srv_cert = load_cert_pwd(opt_srv_cert, opt_srv_keypass,
                                       "certificate of the server");

        if (srv_cert == NULL || !OSSL_CMP_CTX_set1_cert(ctx, srv_cert)) {
            X509_free(srv_cert);
            goto err;
        }
        X509_free(srv_cert);
    }
    if (opt_srv_key != NULL) {
        EVP_PKEY *pkey = load_key_pwd(opt_srv_key, opt_keyform,
                                      opt_srv_keypass,
                                      e, "private key for server cert");

        if (pkey == NULL || !OSSL_CMP_CTX_set1_pkey(ctx, pkey)) {
            EVP_PKEY_free(pkey);
            goto err;
        }
        EVP_PKEY_free(pkey);
    }
    cleanse(opt_srv_keypass);

    if (opt_srv_trusted != NULL) {
        X509_STORE *ts =
            load_certstore(opt_srv_trusted, "certificates trusted by server");

        if (ts == NULL)
            goto err;
        if (!set1_store_parameters(ts)
                || !truststore_set_host_etc(ts, NULL)
                || !OSSL_CMP_CTX_set0_trustedStore(ctx, ts)) {
            X509_STORE_free(ts);
            goto err;
        }
    } else {
        CMP_warn("server will not be able to handle signature-protected requests since -srv_trusted is not given");
    }
    if (!setup_certs(opt_srv_untrusted, "untrusted certificates", ctx,
                     (add_X509_stack_fn_t)OSSL_CMP_CTX_set1_untrusted_certs,
                     NULL))
        goto err;

    if (opt_rsp_cert == NULL) {
        CMP_err("must give -rsp_cert for mock server");
        goto err;
    } else {
        X509 *cert = load_cert_pwd(opt_rsp_cert, opt_keypass,
                                   "cert to be returned by the mock server");

        if (cert == NULL)
            goto err;
        /* from server perspective the server is the client */
        if (!ossl_cmp_mock_srv_set1_certOut(srv_ctx, cert)) {
            X509_free(cert);
            goto err;
        }
        X509_free(cert);
    }
    /* TODO find a cleaner solution not requiring type casts */
    if (!setup_certs(opt_rsp_extracerts,
                     "CMP extra certificates for mock server", srv_ctx,
                     (add_X509_stack_fn_t)ossl_cmp_mock_srv_set1_chainOut,
                     NULL))
        goto err;
    if (!setup_certs(opt_rsp_capubs, "caPubs for mock server", srv_ctx,
                     (add_X509_stack_fn_t)ossl_cmp_mock_srv_set1_caPubsOut,
                     NULL))
        goto err;
    (void)ossl_cmp_mock_srv_set_pollCount(srv_ctx, opt_poll_count);
    (void)ossl_cmp_mock_srv_set_checkAfterTime(srv_ctx, opt_check_after);
    if (opt_grant_implicitconf)
        (void)OSSL_CMP_SRV_CTX_set_grant_implicit_confirm(srv_ctx, 1);

    if (opt_failure != INT_MIN) { /* option has been set explicity */
        if (opt_failure < 0 || OSSL_CMP_PKIFAILUREINFO_MAX < opt_failure) {
            CMP_err1("-failure out of range, should be >= 0 and <= %d",
                     OSSL_CMP_PKIFAILUREINFO_MAX);
            goto err;
        }
        if (opt_failurebits != 0)
            CMP_warn("-failurebits overrides -failure");
        else
            opt_failurebits = 1 << opt_failure;
    }
    if ((unsigned)opt_failurebits > OSSL_CMP_PKIFAILUREINFO_MAX_BIT_PATTERN) {
        CMP_err("-failurebits out of range");
        goto err;
    }
    if (!ossl_cmp_mock_srv_set_statusInfo(srv_ctx, opt_pkistatus,
                                          opt_failurebits, opt_statusstring))
        goto err;

    if (opt_send_error)
        (void)ossl_cmp_mock_srv_set_send_error(srv_ctx, 1);

    if (opt_send_unprotected)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_UNPROTECTED_SEND, 1);
    if (opt_send_unprot_err)
        (void)OSSL_CMP_SRV_CTX_set_send_unprotected_errors(srv_ctx, 1);
    if (opt_accept_unprotected)
        (void)OSSL_CMP_SRV_CTX_set_accept_unprotected(srv_ctx, 1);
    if (opt_accept_unprot_err)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_UNPROTECTED_ERRORS, 1);
    if (opt_accept_raverified)
        (void)OSSL_CMP_SRV_CTX_set_accept_raverified(srv_ctx, 1);

    return srv_ctx;

 err:
    ossl_cmp_mock_srv_free(srv_ctx);
    return NULL;
}

/*
 * set up verification aspects of OSSL_CMP_CTX w.r.t. opts from config file/CLI.
 * Returns pointer on success, NULL on error
 */
static int setup_verification_ctx(OSSL_CMP_CTX *ctx)
{
    if (!setup_certs(opt_untrusted, "untrusted certificates", ctx,
                     (add_X509_stack_fn_t)OSSL_CMP_CTX_set1_untrusted_certs,
                     NULL))
        goto err;

    if (opt_srvcert != NULL || opt_trusted != NULL) {
        X509_STORE *ts = NULL;

        if (opt_srvcert != NULL) {
            X509 *srvcert;

            if (opt_trusted != NULL) {
                CMP_warn("-trusted option is ignored since -srvcert option is present");
                opt_trusted = NULL;
            }
            if (opt_recipient != NULL) {
                CMP_warn("-recipient option is ignored since -srvcert option is present");
                opt_recipient = NULL;
            }
            srvcert = load_cert_pwd(opt_srvcert, opt_otherpass,
                                    "directly trusted CMP server certificate");
            if (srvcert == NULL)
                /*
                 * opt_otherpass is needed in case
                 * opt_srvcert is an encrypted PKCS#12 file
                 */
                goto err;
            if (!OSSL_CMP_CTX_set1_srvCert(ctx, srvcert)) {
                X509_free(srvcert);
                goto oom;
            }
            X509_free(srvcert);
            if ((ts = X509_STORE_new()) == NULL)
                goto oom;
        }
        if (opt_trusted != NULL
                && (ts = load_certstore(opt_trusted, "trusted certificates"))
            == NULL)
            goto err;
        if (!set1_store_parameters(ts) /* also copies vpm */
                /*
                 * clear any expected host/ip/email address;
                 * opt_expect_sender is used instead
                 */
                || !truststore_set_host_etc(ts, NULL)
                || !OSSL_CMP_CTX_set0_trustedStore(ctx, ts)) {
            X509_STORE_free(ts);
            goto oom;
        }
    }

    if (opt_ignore_keyusage)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_IGNORE_KEYUSAGE, 1);

    if (opt_unprotected_errors)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_UNPROTECTED_ERRORS, 1);

    if (opt_out_trusted != NULL) { /* for use in OSSL_CMP_certConf_cb() */
        X509_VERIFY_PARAM *out_vpm = NULL;
        X509_STORE *out_trusted =
            load_certstore(opt_out_trusted,
                           "trusted certs for verifying newly enrolled cert");

        if (out_trusted == NULL)
            goto err;
        /* any -verify_hostname, -verify_ip, and -verify_email apply here */
        if (!set1_store_parameters(out_trusted))
            goto oom;
        /* ignore any -attime here, new certs are current anyway */
        out_vpm = X509_STORE_get0_param(out_trusted);
        X509_VERIFY_PARAM_clear_flags(out_vpm, X509_V_FLAG_USE_CHECK_TIME);

        (void)OSSL_CMP_CTX_set_certConf_cb_arg(ctx, out_trusted);
    }

    if (opt_disable_confirm)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_DISABLE_CONFIRM, 1);

    if (opt_implicit_confirm)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_IMPLICIT_CONFIRM, 1);

    (void)OSSL_CMP_CTX_set_certConf_cb(ctx, OSSL_CMP_certConf_cb);

    return 1;

 oom:
    CMP_err("out of memory");
 err:
    return 0;
}

#ifndef OPENSSL_NO_SOCK
/*
 * set up ssl_ctx for the OSSL_CMP_CTX based on options from config file/CLI.
 * Returns pointer on success, NULL on error
 */
static SSL_CTX *setup_ssl_ctx(OSSL_CMP_CTX *ctx, ENGINE *e)
{
    STACK_OF(X509) *untrusted_certs = OSSL_CMP_CTX_get0_untrusted_certs(ctx);
    EVP_PKEY *pkey = NULL;
    X509_STORE *trust_store = NULL;
    SSL_CTX *ssl_ctx;
    int i;

    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (ssl_ctx == NULL)
        return NULL;

    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);

    if (opt_tls_trusted != NULL) {
        if ((trust_store = load_certstore(opt_tls_trusted,
                                          "trusted TLS certificates")) == NULL)
            goto err;
        SSL_CTX_set_cert_store(ssl_ctx, trust_store);
        /* for improved diagnostics on SSL_CTX_build_cert_chain() errors: */
        X509_STORE_set_verify_cb(trust_store, X509_STORE_CTX_print_verify_cb);
    }

    if (opt_tls_cert != NULL && opt_tls_key != NULL) {
        X509 *cert;
        STACK_OF(X509) *certs = NULL;

        if (!load_certs_autofmt(opt_tls_cert, &certs, 0, opt_tls_keypass,
                                "TLS client certificate (optionally with chain)"))
            /*
             * opt_tls_keypass is needed in case opt_tls_cert is an encrypted
             * PKCS#12 file
             */
            goto err;

        cert = sk_X509_delete(certs, 0);
        if (cert == NULL || SSL_CTX_use_certificate(ssl_ctx, cert) <= 0) {
            CMP_err1("unable to use client TLS certificate file '%s'",
                     opt_tls_cert);
            X509_free(cert);
            sk_X509_pop_free(certs, X509_free);
            goto err;
        }
        X509_free(cert); /* we do not need the handle any more */

        /*
         * Any further certs and any untrusted certs are used for constructing
         * the client cert chain to be provided along with the TLS client cert
         * to the TLS server.
         */
        if (!SSL_CTX_set0_chain(ssl_ctx, certs)) {
            CMP_err("could not set TLS client cert chain");
            sk_X509_pop_free(certs, X509_free);
            goto err;
        }
        for (i = 0; i < sk_X509_num(untrusted_certs); i++) {
            cert = sk_X509_value(untrusted_certs, i);
            if (!SSL_CTX_add1_chain_cert(ssl_ctx, cert)) {
                CMP_err("could not add untrusted cert to TLS client cert chain");
                goto err;
            }
        }
        if (!SSL_CTX_build_cert_chain(ssl_ctx,
                                      SSL_BUILD_CHAIN_FLAG_UNTRUSTED |
                                      SSL_BUILD_CHAIN_FLAG_NO_ROOT)) {
            CMP_warn("could not build cert chain for own TLS cert");
            OSSL_CMP_CTX_print_errors(ctx);
        }

        /* If present we append to the list also the certs from opt_tls_extra */
        if (opt_tls_extra != NULL) {
            STACK_OF(X509) *tls_extra = load_certs_multifile(opt_tls_extra,
                                                             opt_otherpass,
                                                             "extra certificates for TLS");
            int res = 1;

            if (tls_extra == NULL)
                goto err;
            for (i = 0; i < sk_X509_num(tls_extra); i++) {
                cert = sk_X509_value(tls_extra, i);
                if (res != 0)
                    res = SSL_CTX_add_extra_chain_cert(ssl_ctx, cert);
                if (res == 0)
                    X509_free(cert);
            }
            sk_X509_free(tls_extra);
            if (res == 0) {
                BIO_printf(bio_err, "error: unable to add TLS extra certs\n");
                goto err;
            }
        }

        pkey = load_key_pwd(opt_tls_key, opt_keyform, opt_tls_keypass,
                            e, "TLS client private key");
        cleanse(opt_tls_keypass);
        if (pkey == NULL)
            goto err;
        /*
         * verify the key matches the cert,
         * not using SSL_CTX_check_private_key(ssl_ctx)
         * because it gives poor and sometimes misleading diagnostics
         */
        if (!X509_check_private_key(SSL_CTX_get0_certificate(ssl_ctx),
                                    pkey)) {
            CMP_err2("TLS private key '%s' does not match the TLS certificate '%s'\n",
                     opt_tls_key, opt_tls_cert);
            EVP_PKEY_free(pkey);
            pkey = NULL; /* otherwise, for some reason double free! */
            goto err;
        }
        if (SSL_CTX_use_PrivateKey(ssl_ctx, pkey) <= 0) {
            CMP_err1("unable to use TLS client private key '%s'", opt_tls_key);
            EVP_PKEY_free(pkey);
            pkey = NULL; /* otherwise, for some reason double free! */
            goto err;
        }
        EVP_PKEY_free(pkey); /* we do not need the handle any more */
    }
    if (opt_tls_trusted != NULL) {
        /* enable and parameterize server hostname/IP address check */
        if (!truststore_set_host_etc(trust_store,
                                     opt_tls_host != NULL ?
                                     opt_tls_host : opt_server))
            /* TODO: is the server host name correct for TLS via proxy? */
            goto err;
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    }
    return ssl_ctx;
 err:
    SSL_CTX_free(ssl_ctx);
    return NULL;
}
#endif

/*
 * set up protection aspects of OSSL_CMP_CTX based on options from config
 * file/CLI while parsing options and checking their consistency.
 * Returns 1 on success, 0 on error
 */
static int setup_protection_ctx(OSSL_CMP_CTX *ctx, ENGINE *e)
{
    if (!opt_unprotected_requests && opt_secret == NULL && opt_cert == NULL) {
        CMP_err("must give client credentials unless -unprotected_requests is set");
        goto err;
    }

    if (opt_ref == NULL && opt_cert == NULL && opt_subject == NULL) {
        /* cert or subject should determine the sender */
        CMP_err("must give -ref if no -cert and no -subject given");
        goto err;
    }
    if (!opt_secret && ((opt_cert == NULL) != (opt_key == NULL))) {
        CMP_err("must give both -cert and -key options or neither");
        goto err;
    }
    if (opt_secret != NULL) {
        char *pass_string = get_passwd(opt_secret, "PBMAC");
        int res;

        if (pass_string != NULL) {
            cleanse(opt_secret);
            res = OSSL_CMP_CTX_set1_secretValue(ctx,
                                                (unsigned char *)pass_string,
                                                strlen(pass_string));
            clear_free(pass_string);
            if (res == 0)
                goto err;
        }
        if (opt_cert != NULL || opt_key != NULL)
            CMP_warn("no signature-based protection used since -secret is given");
    }
    if (opt_ref != NULL
            && !OSSL_CMP_CTX_set1_referenceValue(ctx, (unsigned char *)opt_ref,
                                                 strlen(opt_ref)))
        goto err;

    if (opt_key != NULL) {
        EVP_PKEY *pkey = load_key_pwd(opt_key, opt_keyform, opt_keypass, e,
                                      "private key for CMP client certificate");

        if (pkey == NULL || !OSSL_CMP_CTX_set1_pkey(ctx, pkey)) {
            EVP_PKEY_free(pkey);
            goto err;
        }
        EVP_PKEY_free(pkey);
    }
    if (opt_secret == NULL && opt_srvcert == NULL && opt_trusted == NULL) {
        CMP_err("missing -secret or -srvcert or -trusted");
        goto err;
    }

    if (opt_cert != NULL) {
        X509 *cert;
        STACK_OF(X509) *certs = NULL;
        int ok;

        if (!load_certs_autofmt(opt_cert, &certs, 0, opt_keypass,
                                "CMP client certificate (and optionally extra certs)"))
            /* opt_keypass is needed if opt_cert is an encrypted PKCS#12 file */
            goto err;

        cert = sk_X509_delete(certs, 0);
        if (cert == NULL) {
            CMP_err("no client certificate found");
            sk_X509_pop_free(certs, X509_free);
            goto err;
        }
        ok = OSSL_CMP_CTX_set1_cert(ctx, cert);
        X509_free(cert);

        if (ok) {
            /* add any remaining certs to the list of untrusted certs */
            STACK_OF(X509) *untrusted = OSSL_CMP_CTX_get0_untrusted_certs(ctx);
            ok = untrusted != NULL ?
                sk_X509_add1_certs(untrusted, certs, 0, 1 /* no dups */, 0) :
                OSSL_CMP_CTX_set1_untrusted_certs(ctx, certs);
        }
        sk_X509_pop_free(certs, X509_free);
        if (!ok)
            goto oom;
    }

    if (!setup_certs(opt_extracerts, "extra certificates for CMP", ctx,
                     (add_X509_stack_fn_t)OSSL_CMP_CTX_set1_extraCertsOut,
                     NULL))
        goto err;
    cleanse(opt_otherpass);

    if (opt_unprotected_requests)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_UNPROTECTED_SEND, 1);

    if (opt_digest != NULL) {
        int digest = OBJ_ln2nid(opt_digest);

        if (digest == NID_undef) {
            CMP_err1("digest algorithm name not recognized: '%s'", opt_digest);
            goto err;
        }
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_DIGEST_ALGNID, digest);
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_OWF_ALGNID, digest);
    }

    if (opt_mac != NULL) {
        int mac = OBJ_ln2nid(opt_mac);
        if (mac == NID_undef) {
            CMP_err1("MAC algorithm name not recognized: '%s'", opt_mac);
            goto err;
        }
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_MAC_ALGNID, mac);
    }
    return 1;

 oom:
    CMP_err("out of memory");
 err:
    return 0;
}

/*
 * set up IR/CR/KUR/CertConf/RR specific parts of the OSSL_CMP_CTX
 * based on options from config file/CLI.
 * Returns pointer on success, NULL on error
 */
static int setup_request_ctx(OSSL_CMP_CTX *ctx, ENGINE *e)
{
    if (opt_subject == NULL && opt_oldcert == NULL && opt_cert == NULL)
        CMP_warn("no -subject given, neither -oldcert nor -cert available as default");
    if (!set_name(opt_subject, OSSL_CMP_CTX_set1_subjectName, ctx, "subject")
            || !set_name(opt_issuer, OSSL_CMP_CTX_set1_issuer, ctx, "issuer"))
        goto err;

    if (opt_newkey != NULL) {
        const char *file = opt_newkey;
        const int format = opt_keyform;
        const char *pass = opt_newkeypass;
        const char *desc = "new private or public key for cert to be enrolled";
        EVP_PKEY *pkey = load_key_pwd(file, format, pass, e, NULL);
        int priv = 1;

        if (pkey == NULL) {
            ERR_clear_error();
            pkey = load_pubkey(file, format, 0, pass, e, desc);
            priv = 0;
        }
        cleanse(opt_newkeypass);
        if (pkey == NULL || !OSSL_CMP_CTX_set0_newPkey(ctx, priv, pkey)) {
            EVP_PKEY_free(pkey);
            goto err;
        }
    }

    if (opt_days > 0)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_VALIDITY_DAYS,
                                      opt_days);

    if (opt_policies != NULL && opt_policy_oids != NULL) {
        CMP_err("cannot have policies both via -policies and via -policy_oids");
        goto err;
    }

    if (opt_reqexts != NULL || opt_policies != NULL) {
        X509V3_CTX ext_ctx;
        X509_EXTENSIONS *exts = sk_X509_EXTENSION_new_null();

        if (exts == NULL)
            goto err;
        X509V3_set_ctx(&ext_ctx, NULL, NULL, NULL, NULL, 0);
        X509V3_set_nconf(&ext_ctx, conf);
        if (opt_reqexts != NULL
            && !X509V3_EXT_add_nconf_sk(conf, &ext_ctx, opt_reqexts, &exts)) {
            CMP_err1("cannot load certificate request extension section '%s'",
                     opt_reqexts);
            sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
            goto err;
        }
        if (opt_policies != NULL
            && !X509V3_EXT_add_nconf_sk(conf, &ext_ctx, opt_policies, &exts)) {
            CMP_err1("cannot load policy cert request extension section '%s'",
                     opt_policies);
            sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
            goto err;
        }
        OSSL_CMP_CTX_set0_reqExtensions(ctx, exts);
    }
    if (OSSL_CMP_CTX_reqExtensions_have_SAN(ctx) && opt_sans != NULL) {
        CMP_err("cannot have Subject Alternative Names both via -reqexts and via -sans");
        goto err;
    }

    if (!set_gennames(ctx, opt_sans, "Subject Alternative Name"))
        goto err;

    if (opt_san_nodefault) {
        if (opt_sans != NULL)
            CMP_warn("-opt_san_nodefault has no effect when -sans is used");
        (void)OSSL_CMP_CTX_set_option(ctx,
                                      OSSL_CMP_OPT_SUBJECTALTNAME_NODEFAULT, 1);
    }

    if (opt_policy_oids_critical) {
        if (opt_policy_oids == NULL)
            CMP_warn("-opt_policy_oids_critical has no effect unless -policy_oids is given");
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_POLICIES_CRITICAL, 1);
    }

    while (opt_policy_oids != NULL) {
        ASN1_OBJECT *policy;
        POLICYINFO *pinfo;
        char *next = next_item(opt_policy_oids);

        if ((policy = OBJ_txt2obj(opt_policy_oids, 1)) == 0) {
            CMP_err1("unknown policy OID '%s'", opt_policy_oids);
            goto err;
        }

        if ((pinfo = POLICYINFO_new()) == NULL) {
            ASN1_OBJECT_free(policy);
            goto err;
        }
        pinfo->policyid = policy;

        if (!OSSL_CMP_CTX_push0_policy(ctx, pinfo)) {
            CMP_err1("cannot add policy with OID '%s'", opt_policy_oids);
            POLICYINFO_free(pinfo);
            goto err;
        }
        opt_policy_oids = next;
    }

    if (opt_popo >= OSSL_CRMF_POPO_NONE)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_POPO_METHOD, opt_popo);

    if (opt_csr != NULL) {
        if (opt_cmd != CMP_P10CR) {
            CMP_warn("-csr option is ignored for command other than p10cr");
        } else {
            X509_REQ *csr =
                load_csr_autofmt(opt_csr, "PKCS#10 CSR for p10cr");

            if (csr == NULL)
                goto err;
            if (!OSSL_CMP_CTX_set1_p10CSR(ctx, csr)) {
                X509_REQ_free(csr);
                goto oom;
            }
            X509_REQ_free(csr);
        }
    }

    if (opt_oldcert != NULL) {
        X509 *oldcert = load_cert_pwd(opt_oldcert, opt_keypass,
                                      "certificate to be updated/revoked");
        /* opt_keypass is needed if opt_oldcert is an encrypted PKCS#12 file */

        if (oldcert == NULL)
            goto err;
        if (!OSSL_CMP_CTX_set1_oldCert(ctx, oldcert)) {
            X509_free(oldcert);
            goto oom;
        }
        X509_free(oldcert);
    }
    cleanse(opt_keypass);
    if (opt_revreason > CRL_REASON_NONE)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_REVOCATION_REASON,
                                      opt_revreason);

    return 1;

 oom:
    CMP_err("out of memory");
 err:
    return 0;
}

static int handle_opt_geninfo(OSSL_CMP_CTX *ctx)
{
    long value;
    ASN1_OBJECT *type;
    ASN1_INTEGER *aint;
    ASN1_TYPE *val;
    OSSL_CMP_ITAV *itav;
    char *endstr;
    char *valptr = strchr(opt_geninfo, ':');

    if (valptr == NULL) {
        CMP_err("missing ':' in -geninfo option");
        return 0;
    }
    valptr[0] = '\0';
    valptr++;

    if (strncasecmp(valptr, "int:", 4) != 0) {
        CMP_err("missing 'int:' in -geninfo option");
        return 0;
    }
    valptr += 4;

    value = strtol(valptr, &endstr, 10);
    if (endstr == valptr || *endstr != '\0') {
        CMP_err("cannot parse int in -geninfo option");
        return 0;
    }

    type = OBJ_txt2obj(opt_geninfo, 1);
    if (type == NULL) {
        CMP_err("cannot parse OID in -geninfo option");
        return 0;
    }

    aint = ASN1_INTEGER_new();
    if (aint == NULL || !ASN1_INTEGER_set(aint, value))
        goto oom;

    val = ASN1_TYPE_new();
    if (val == NULL) {
        ASN1_INTEGER_free(aint);
        goto oom;
    }
    ASN1_TYPE_set(val, V_ASN1_INTEGER, aint);
    itav = OSSL_CMP_ITAV_create(type, val);
    if (itav == NULL) {
        ASN1_TYPE_free(val);
        goto oom;
    }

    if (!OSSL_CMP_CTX_push0_geninfo_ITAV(ctx, itav)) {
        OSSL_CMP_ITAV_free(itav);
        return 0;
    }
    return 1;

 oom:
    CMP_err("out of memory");
    return 0;
}


/*
 * set up the client-side OSSL_CMP_CTX based on options from config file/CLI
 * while parsing options and checking their consistency.
 * Prints reason for error to bio_err.
 * Returns 1 on success, 0 on error
 */
static int setup_client_ctx(OSSL_CMP_CTX *ctx, ENGINE *e)
{
    int ret = 0;
    char server_buf[200] = { '\0' };
    char proxy_buf[200] = { '\0' };
    char *proxy_host = NULL;
    char *proxy_port_str = NULL;

    if (opt_server == NULL) {
        CMP_err("missing server address[:port]");
        goto err;
    } else if ((server_port =
                parse_addr(&opt_server, server_port, "server")) < 0) {
        goto err;
    }
    if (server_port != 0)
        BIO_snprintf(server_port_s, sizeof(server_port_s), "%d", server_port);
    if (!OSSL_CMP_CTX_set1_server(ctx, opt_server)
            || !OSSL_CMP_CTX_set_serverPort(ctx, server_port)
            || !OSSL_CMP_CTX_set1_serverPath(ctx, opt_path))
        goto oom;
    if (opt_proxy != NULL && !OSSL_CMP_CTX_set1_proxy(ctx, opt_proxy))
        goto oom;
    (void)BIO_snprintf(server_buf, sizeof(server_buf), "http%s://%s%s%s/%s",
                       opt_tls_used ? "s" : "", opt_server,
                       server_port == 0 ? "" : ":", server_port_s,
                       opt_path[0] == '/' ? opt_path + 1 : opt_path);

    if (opt_proxy != NULL)
        (void)BIO_snprintf(proxy_buf, sizeof(proxy_buf), " via %s", opt_proxy);
    CMP_info2("will contact %s%s", server_buf, proxy_buf);

    if (!transform_opts())
        goto err;

    if (opt_cmd == CMP_IR || opt_cmd == CMP_CR || opt_cmd == CMP_KUR) {
        if (opt_newkey == NULL && opt_key == NULL && opt_csr == NULL) {
            CMP_err("missing -newkey (or -key) to be certified");
            goto err;
        }
        if (opt_certout == NULL) {
            CMP_err("-certout not given, nowhere to save certificate");
            goto err;
        }
    }
    if (opt_cmd == CMP_KUR) {
        char *ref_cert = opt_oldcert != NULL ? opt_oldcert : opt_cert;

        if (ref_cert == NULL) {
            CMP_err("missing -oldcert option for certificate to be updated");
            goto err;
        }
        if (opt_subject != NULL)
            CMP_warn2("-subject '%s' given, which overrides the subject of '%s' in KUR",
                      opt_subject, ref_cert);
    }
    if (opt_cmd == CMP_RR && opt_oldcert == NULL) {
        CMP_err("missing certificate to be revoked");
        goto err;
    }
    if (opt_cmd == CMP_P10CR && opt_csr == NULL) {
        CMP_err("missing PKCS#10 CSR for p10cr");
        goto err;
    }

    if (opt_recipient == NULL && opt_srvcert == NULL && opt_issuer == NULL
            && opt_oldcert == NULL && opt_cert == NULL)
        CMP_warn("missing -recipient, -srvcert, -issuer, -oldcert or -cert; recipient will be set to \"NULL-DN\"");

    if (opt_infotype_s != NULL) {
        char id_buf[100] = "id-it-";

        strncat(id_buf, opt_infotype_s, sizeof(id_buf) - strlen(id_buf) - 1);
        if ((opt_infotype = OBJ_sn2nid(id_buf)) == NID_undef) {
            CMP_err("unknown OID name in -infotype option");
            goto err;
        }
    }

    if (!setup_verification_ctx(ctx))
        goto err;

    if (opt_msg_timeout >= 0) /* must do this before setup_ssl_ctx() */
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_MSG_TIMEOUT,
                                      opt_msg_timeout);
    if (opt_total_timeout >= 0)
        (void)OSSL_CMP_CTX_set_option(ctx, OSSL_CMP_OPT_TOTAL_TIMEOUT,
                                      opt_total_timeout);

    if (opt_reqin != NULL && opt_rspin != NULL)
        CMP_warn("-reqin is ignored since -rspin is present");
    if (opt_reqin_new_tid && opt_reqin == NULL)
        CMP_warn("-reqin_new_tid is ignored since -reqin is not present");
    if (opt_reqin != NULL || opt_reqout != NULL
            || opt_rspin != NULL || opt_rspout != NULL || opt_use_mock_srv)
        (void)OSSL_CMP_CTX_set_transfer_cb(ctx, read_write_req_resp);

    if ((opt_tls_cert != NULL || opt_tls_key != NULL
         || opt_tls_keypass != NULL || opt_tls_extra != NULL
         || opt_tls_trusted != NULL || opt_tls_host != NULL)
            && !opt_tls_used)
        CMP_warn("TLS options(s) given but not -tls_used");
    if (opt_tls_used) {
#ifdef OPENSSL_NO_SOCK
        BIO_printf(bio_err, "Cannot use TLS - sockets not supported\n");
        goto err;
#else
        APP_HTTP_TLS_INFO *info;

        if (opt_tls_cert != NULL
            || opt_tls_key != NULL || opt_tls_keypass != NULL) {
            if (opt_tls_key == NULL) {
                CMP_err("missing -tls_key option");
                goto err;
            } else if (opt_tls_cert == NULL) {
                CMP_err("missing -tls_cert option");
                goto err;
            }
        }
        if (opt_use_mock_srv) {
            CMP_err("cannot use TLS options together with -use_mock_srv");
            goto err;
        }
        if ((info = OPENSSL_zalloc(sizeof(*info))) == NULL)
            goto err;
        (void)OSSL_CMP_CTX_set_http_cb_arg(ctx, info);
        /* info will be freed along with CMP ctx */
        info->server = opt_server;
        info->port = server_port_s;
        info->use_proxy = opt_proxy != NULL;
        info->timeout = OSSL_CMP_CTX_get_option(ctx, OSSL_CMP_OPT_MSG_TIMEOUT);
        info->ssl_ctx = setup_ssl_ctx(ctx, e);
        if (info->ssl_ctx == NULL)
            goto err;
        (void)OSSL_CMP_CTX_set_http_cb(ctx, app_http_tls_cb);
#endif
    }

    if (!setup_protection_ctx(ctx, e))
        goto err;

    if (!setup_request_ctx(ctx, e))
        goto err;

    if (!set_name(opt_recipient, OSSL_CMP_CTX_set1_recipient, ctx, "recipient")
            || !set_name(opt_expect_sender, OSSL_CMP_CTX_set1_expected_sender,
                         ctx, "expected sender"))
        goto oom;

    if (opt_geninfo != NULL && !handle_opt_geninfo(ctx))
        goto err;

    ret = 1;

 err:
    OPENSSL_free(proxy_host);
    OPENSSL_free(proxy_port_str);
    return ret;
 oom:
    CMP_err("out of memory");
    goto err;
}

/*
 * write out the given certificate to the output specified by bio.
 * Depending on options use either PEM or DER format.
 * Returns 1 on success, 0 on error
 */
static int write_cert(BIO *bio, X509 *cert)
{
    if ((opt_certform == FORMAT_PEM && PEM_write_bio_X509(bio, cert))
            || (opt_certform == FORMAT_ASN1 && i2d_X509_bio(bio, cert)))
        return 1;
    if (opt_certform != FORMAT_PEM && opt_certform != FORMAT_ASN1)
        BIO_printf(bio_err,
                   "error: unsupported type '%s' for writing certificates\n",
                   opt_certform_s);
    return 0;
}

/*
 * writes out a stack of certs to the given file.
 * Depending on options use either PEM or DER format,
 * where DER does not make much sense for writing more than one cert!
 * Returns number of written certificates on success, 0 on error.
 */
static int save_certs(OSSL_CMP_CTX *ctx,
                      STACK_OF(X509) *certs, char *destFile, char *desc)
{
    BIO *bio = NULL;
    int i;
    int n = sk_X509_num(certs);

    CMP_info3("received %d %s certificate(s), saving to file '%s'",
              n, desc, destFile);
    if (n > 1 && opt_certform != FORMAT_PEM)
        CMP_warn("saving more than one certificate in non-PEM format");

    if (destFile == NULL || (bio = BIO_new(BIO_s_file())) == NULL
            || !BIO_write_filename(bio, (char *)destFile)) {
        CMP_err1("could not open file '%s' for writing", destFile);
        n = -1;
        goto err;
    }

    for (i = 0; i < n; i++) {
        if (!write_cert(bio, sk_X509_value(certs, i))) {
            CMP_err1("cannot write certificate to file '%s'", destFile);
            n = -1;
            goto err;
        }
    }

 err:
    BIO_free(bio);
    return n;
}

static void print_itavs(STACK_OF(OSSL_CMP_ITAV) *itavs)
{
    OSSL_CMP_ITAV *itav = NULL;
    char buf[128];
    int i;
    int n = sk_OSSL_CMP_ITAV_num(itavs); /* itavs == NULL leads to 0 */

    if (n == 0) {
        CMP_info("genp contains no ITAV");
        return;
    }

    for (i = 0; i < n; i++) {
        itav = sk_OSSL_CMP_ITAV_value(itavs, i);
        OBJ_obj2txt(buf, 128, OSSL_CMP_ITAV_get0_type(itav), 0);
        CMP_info1("genp contains ITAV of type: %s", buf);
    }
}

static char opt_item[SECTION_NAME_MAX + 1];
/* get previous name from a comma-separated list of names */
static const char *prev_item(const char *opt, const char *end)
{
    const char *beg;
    size_t len;

    if (end == opt)
        return NULL;
    beg = end;
    while (beg != opt && beg[-1] != ',' && !isspace(beg[-1]))
        beg--;
    len = end - beg;
    if (len > SECTION_NAME_MAX)
        len = SECTION_NAME_MAX;
    strncpy(opt_item, beg, len);
    opt_item[SECTION_NAME_MAX] = '\0'; /* avoid gcc v8 O3 stringop-truncation */
    opt_item[len] = '\0';
    if (len > SECTION_NAME_MAX)
        CMP_warn2("using only first %d characters of section name starting with \"%s\"",
                  SECTION_NAME_MAX, opt_item);
    while (beg != opt && (beg[-1] == ',' || isspace(beg[-1])))
        beg--;
    return beg;
}

/* get str value for name from a comma-separated hierarchy of config sections */
static char *conf_get_string(const CONF *src_conf, const char *groups,
                             const char *name)
{
    char *res = NULL;
    const char *end = groups + strlen(groups);

    while ((end = prev_item(groups, end)) != NULL) {
        if ((res = NCONF_get_string(src_conf, opt_item, name)) != NULL)
            return res;
    }
    return res;
}

/* get long val for name from a comma-separated hierarchy of config sections */
static int conf_get_number_e(const CONF *conf_, const char *groups,
                             const char *name, long *result)
{
    char *str = conf_get_string(conf_, groups, name);
    char *tailptr;
    long res;

    if (str == NULL || *str == '\0')
        return 0;

    res = strtol(str, &tailptr, 10);
    if (res == LONG_MIN || res == LONG_MAX || *tailptr != '\0')
        return 0;

    *result = res;
    return 1;
}

/*
 * use the command line option table to read values from the CMP section
 * of openssl.cnf.  Defaults are taken from the config file, they can be
 * overwritten on the command line.
 */
static int read_config(void)
{
    unsigned int i;
    long num = 0;
    char *txt = NULL;
    const OPTIONS *opt;
    int provider_option;
    int verification_option;

    /*
     * starting with offset OPT_SECTION because OPT_CONFIG and OPT_SECTION would
     * not make sense within the config file. They have already been handled.
     */
    for (i = OPT_SECTION - OPT_HELP, opt = &cmp_options[OPT_SECTION];
         opt->name; i++, opt++) {
        if (!strcmp(opt->name, OPT_SECTION_STR)
                || !strcmp(opt->name, OPT_MORE_STR)) {
            i--;
            continue;
        }
        provider_option = (OPT_PROV__FIRST <= opt->retval
                               && opt->retval < OPT_PROV__LAST);
        verification_option = (OPT_V__FIRST <= opt->retval
                                   && opt->retval < OPT_V__LAST);
        if (provider_option || verification_option)
            i--;
        if (cmp_vars[i].txt == NULL) {
            CMP_err1("internal: cmp_vars array too short, i=%d", i);
            return 0;
        }
        switch (opt->valtype) {
        case '-':
        case 'n':
        case 'l':
            if (!conf_get_number_e(conf, opt_section, opt->name, &num)) {
                ERR_clear_error();
                continue; /* option not provided */
            }
            break;
            /*
             * do not use '<' in cmp_options. Incorrect treatment
             * somewhere in args_verify() can wrongly set badarg = 1
             */
        case '<':
        case 's':
        case 'M':
            txt = conf_get_string(conf, opt_section, opt->name);
            if (txt == NULL) {
                ERR_clear_error();
                continue; /* option not provided */
            }
            break;
        default:
            CMP_err2("internal: unsupported type '%c' for option '%s'",
                     opt->valtype, opt->name);
            return 0;
            break;
        }
        if (provider_option || verification_option) {
            int conf_argc = 1;
            char *conf_argv[3];
            char arg1[82];

            BIO_snprintf(arg1, 81, "-%s", (char *)opt->name);
            conf_argv[0] = prog;
            conf_argv[1] = arg1;
            if (opt->valtype == '-') {
                if (num != 0)
                    conf_argc = 2;
            } else {
                conf_argc = 3;
                conf_argv[2] = conf_get_string(conf, opt_section, opt->name);
                /* not NULL */
            }
            if (conf_argc > 1) {
                (void)opt_init(conf_argc, conf_argv, cmp_options);

                if (provider_option
                    ? !opt_provider(opt_next())
                    : !opt_verify(opt_next(), vpm)) {
                    CMP_err2("for option '%s' in config file section '%s'",
                             opt->name, opt_section);
                    return 0;
                }
            }
        } else {
            switch (opt->valtype) {
            case '-':
            case 'n':
                if (num < INT_MIN || INT_MAX < num) {
                    BIO_printf(bio_err,
                               "integer value out of range for option '%s'\n",
                               opt->name);
                    return 0;
                }
                *cmp_vars[i].num = (int)num;
                break;
            case 'l':
                *cmp_vars[i].num_long = num;
                break;
            default:
                if (txt != NULL && txt[0] == '\0')
                    txt = NULL; /* reset option on empty string input */
                *cmp_vars[i].txt = txt;
                break;
            }
        }
    }

    return 1;
}

static char *opt_str(char *opt)
{
    char *arg = opt_arg();

    if (arg[0] == '\0') {
        CMP_warn1("argument of -%s option is empty string, resetting option",
                  opt);
        arg = NULL;
    } else if (arg[0] == '-') {
        CMP_warn1("argument of -%s option starts with hyphen", opt);
    }
    return arg;
}

static int opt_nat(void)
{
    int result = -1;

    if (opt_int(opt_arg(), &result) && result < 0)
        BIO_printf(bio_err, "error: argument '%s' must not be negative\n",
                   opt_arg());
    return result;
}

/* returns 1 on success, 0 on error, -1 on -help (i.e., stop with success) */
static int get_opts(int argc, char **argv)
{
    OPTION_CHOICE o;

    prog = opt_init(argc, argv, cmp_options);

    while ((o = opt_next()) != OPT_EOF) {
        switch (o) {
        case OPT_EOF:
        case OPT_ERR:
            goto opt_err;
        case OPT_HELP:
            opt_help(cmp_options);
            return -1;
        case OPT_CONFIG: /* has already been handled */
            break;
        case OPT_SECTION: /* has already been handled */
            break;
        case OPT_SERVER:
            opt_server = opt_str("server");
            break;
        case OPT_PROXY:
            opt_proxy = opt_str("proxy");
            break;
        case OPT_NO_PROXY:
            opt_no_proxy = opt_str("no_proxy");
            break;
        case OPT_PATH:
            opt_path = opt_str("path");
            break;
        case OPT_MSG_TIMEOUT:
            if ((opt_msg_timeout = opt_nat()) < 0)
                goto opt_err;
            break;
        case OPT_TOTAL_TIMEOUT:
            if ((opt_total_timeout = opt_nat()) < 0)
                goto opt_err;
            break;
        case OPT_TLS_USED:
            opt_tls_used = 1;
            break;
        case OPT_TLS_CERT:
            opt_tls_cert = opt_str("tls_cert");
            break;
        case OPT_TLS_KEY:
            opt_tls_key = opt_str("tls_key");
            break;
        case OPT_TLS_KEYPASS:
            opt_tls_keypass = opt_str("tls_keypass");
            break;
        case OPT_TLS_EXTRA:
            opt_tls_extra = opt_str("tls_extra");
            break;
        case OPT_TLS_TRUSTED:
            opt_tls_trusted = opt_str("tls_trusted");
            break;
        case OPT_TLS_HOST:
            opt_tls_host = opt_str("tls_host");
            break;
        case OPT_REF:
            opt_ref = opt_str("ref");
            break;
        case OPT_SECRET:
            opt_secret = opt_str("secret");
            break;
        case OPT_CERT:
            opt_cert = opt_str("cert");
            break;
        case OPT_KEY:
            opt_key = opt_str("key");
            break;
        case OPT_KEYPASS:
            opt_keypass = opt_str("keypass");
            break;
        case OPT_DIGEST:
            opt_digest = opt_str("digest");
            break;
        case OPT_MAC:
            opt_mac = opt_str("mac");
            break;
        case OPT_EXTRACERTS:
            opt_extracerts = opt_str("extracerts");
            break;
        case OPT_UNPROTECTED_REQUESTS:
            opt_unprotected_requests = 1;
            break;

        case OPT_TRUSTED:
            opt_trusted = opt_str("trusted");
            break;
        case OPT_UNTRUSTED:
            opt_untrusted = opt_str("untrusted");
            break;
        case OPT_SRVCERT:
            opt_srvcert = opt_str("srvcert");
            break;
        case OPT_RECIPIENT:
            opt_recipient = opt_str("recipient");
            break;
        case OPT_EXPECT_SENDER:
            opt_expect_sender = opt_str("expect_sender");
            break;
        case OPT_IGNORE_KEYUSAGE:
            opt_ignore_keyusage = 1;
            break;
        case OPT_UNPROTECTED_ERRORS:
            opt_unprotected_errors = 1;
            break;
        case OPT_EXTRACERTSOUT:
            opt_extracertsout = opt_str("extracertsout");
            break;
        case OPT_CACERTSOUT:
            opt_cacertsout = opt_str("cacertsout");
            break;

        case OPT_V_CASES:
            if (!opt_verify(o, vpm))
                goto opt_err;
            break;
        case OPT_CMD:
            opt_cmd_s = opt_str("cmd");
            break;
        case OPT_INFOTYPE:
            opt_infotype_s = opt_str("infotype");
            break;
        case OPT_GENINFO:
            opt_geninfo = opt_str("geninfo");
            break;

        case OPT_NEWKEY:
            opt_newkey = opt_str("newkey");
            break;
        case OPT_NEWKEYPASS:
            opt_newkeypass = opt_str("newkeypass");
            break;
        case OPT_SUBJECT:
            opt_subject = opt_str("subject");
            break;
        case OPT_ISSUER:
            opt_issuer = opt_str("issuer");
            break;
        case OPT_DAYS:
            if ((opt_days = opt_nat()) < 0)
                goto opt_err;
            break;
        case OPT_REQEXTS:
            opt_reqexts = opt_str("reqexts");
            break;
        case OPT_SANS:
            opt_sans = opt_str("sans");
            break;
        case OPT_SAN_NODEFAULT:
            opt_san_nodefault = 1;
            break;
        case OPT_POLICIES:
            opt_policies = opt_str("policies");
            break;
        case OPT_POLICY_OIDS:
            opt_policy_oids = opt_str("policy_oids");
            break;
        case OPT_POLICY_OIDS_CRITICAL:
            opt_policy_oids_critical = 1;
            break;
        case OPT_POPO:
            if (!opt_int(opt_arg(), &opt_popo)
                    || opt_popo < OSSL_CRMF_POPO_NONE
                    || opt_popo > OSSL_CRMF_POPO_KEYENC) {
                CMP_err("invalid popo spec. Valid values are -1 .. 2");
                goto opt_err;
            }
            break;
        case OPT_CSR:
            opt_csr = opt_arg();
            break;
        case OPT_OUT_TRUSTED:
            opt_out_trusted = opt_str("out_trusted");
            break;
        case OPT_IMPLICIT_CONFIRM:
            opt_implicit_confirm = 1;
            break;
        case OPT_DISABLE_CONFIRM:
            opt_disable_confirm = 1;
            break;
        case OPT_CERTOUT:
            opt_certout = opt_str("certout");
            break;
        case OPT_OLDCERT:
            opt_oldcert = opt_str("oldcert");
            break;
        case OPT_REVREASON:
            if (!opt_int(opt_arg(), &opt_revreason)
                    || opt_revreason < CRL_REASON_NONE
                    || opt_revreason > CRL_REASON_AA_COMPROMISE
                    || opt_revreason == 7) {
                CMP_err("invalid revreason. Valid values are -1 .. 6, 8 .. 10");
                goto opt_err;
            }
            break;
        case OPT_CERTFORM:
            opt_certform_s = opt_str("certform");
            break;
        case OPT_KEYFORM:
            opt_keyform_s = opt_str("keyform");
            break;
        case OPT_CERTSFORM:
            opt_certsform_s = opt_str("certsform");
            break;
        case OPT_OTHERPASS:
            opt_otherpass = opt_str("otherpass");
            break;
#ifndef OPENSSL_NO_ENGINE
        case OPT_ENGINE:
            opt_engine = opt_str("engine");
            break;
#endif
        case OPT_PROV_CASES:
            if (!opt_provider(o))
                goto opt_err;
            break;

        case OPT_BATCH:
            opt_batch = 1;
            break;
        case OPT_REPEAT:
            opt_repeat = opt_nat();
            break;
        case OPT_REQIN:
            opt_reqin = opt_str("reqin");
            break;
        case OPT_REQIN_NEW_TID:
            opt_reqin_new_tid = 1;
            break;
        case OPT_REQOUT:
            opt_reqout = opt_str("reqout");
            break;
        case OPT_RSPIN:
            opt_rspin = opt_str("rspin");
            break;
        case OPT_RSPOUT:
            opt_rspout = opt_str("rspout");
            break;
        case OPT_USE_MOCK_SRV:
            opt_use_mock_srv = 1;
            break;
        case OPT_PORT:
            opt_port = opt_str("port");
            break;
        case OPT_MAX_MSGS:
            if ((opt_max_msgs = opt_nat()) < 0)
                goto opt_err;
            break;
        case OPT_SRV_REF:
            opt_srv_ref = opt_str("srv_ref");
            break;
        case OPT_SRV_SECRET:
            opt_srv_secret = opt_str("srv_secret");
            break;
        case OPT_SRV_CERT:
            opt_srv_cert = opt_str("srv_cert");
            break;
        case OPT_SRV_KEY:
            opt_srv_key = opt_str("srv_key");
            break;
        case OPT_SRV_KEYPASS:
            opt_srv_keypass = opt_str("srv_keypass");
            break;
        case OPT_SRV_TRUSTED:
            opt_srv_trusted = opt_str("srv_trusted");
            break;
        case OPT_SRV_UNTRUSTED:
            opt_srv_untrusted = opt_str("srv_untrusted");
            break;
        case OPT_RSP_CERT:
            opt_rsp_cert = opt_str("rsp_cert");
            break;
        case OPT_RSP_EXTRACERTS:
            opt_rsp_extracerts = opt_str("rsp_extracerts");
            break;
        case OPT_RSP_CAPUBS:
            opt_rsp_capubs = opt_str("rsp_capubs");
            break;
        case OPT_POLL_COUNT:
            opt_poll_count = opt_nat();
            break;
        case OPT_CHECK_AFTER:
            opt_check_after = opt_nat();
            break;
        case OPT_GRANT_IMPLICITCONF:
            opt_grant_implicitconf = 1;
            break;
        case OPT_PKISTATUS:
            opt_pkistatus = opt_nat();
            break;
        case OPT_FAILURE:
            opt_failure = opt_nat();
            break;
        case OPT_FAILUREBITS:
            opt_failurebits = opt_nat();
            break;
        case OPT_STATUSSTRING:
            opt_statusstring = opt_str("statusstring");
            break;
        case OPT_SEND_ERROR:
            opt_send_error = 1;
            break;
        case OPT_SEND_UNPROTECTED:
            opt_send_unprotected = 1;
            break;
        case OPT_SEND_UNPROT_ERR:
            opt_send_unprot_err = 1;
            break;
        case OPT_ACCEPT_UNPROTECTED:
            opt_accept_unprotected = 1;
            break;
        case OPT_ACCEPT_UNPROT_ERR:
            opt_accept_unprot_err = 1;
            break;
        case OPT_ACCEPT_RAVERIFIED:
            opt_accept_raverified = 1;
            break;
        }
    }
    argc = opt_num_rest();
    argv = opt_rest();
    if (argc != 0) {
        CMP_err1("unknown parameter %s", argv[0]);
        goto opt_err;
    }
    return 1;

 opt_err:
    CMP_err1("use -help for summary of '%s' options", prog);
    return 0;
}

int cmp_main(int argc, char **argv)
{
    char *configfile = NULL;
    int i;
    X509 *newcert = NULL;
    ENGINE *e = NULL;
    char mock_server[] = "mock server:1";
    int ret = 0; /* default: failure */

    if (argc <= 1) {
        opt_help(cmp_options);
        goto err;
    }

    /*
     * handle OPT_CONFIG and OPT_SECTION upfront to take effect for other opts
     */
    for (i = 1; i < argc - 1; i++) {
        if (*argv[i] == '-') {
            if (!strcmp(argv[i] + 1, cmp_options[OPT_CONFIG - OPT_HELP].name))
                opt_config = argv[i + 1];
            else if (!strcmp(argv[i] + 1,
                             cmp_options[OPT_SECTION - OPT_HELP].name))
                opt_section = argv[i + 1];
        }
    }
    if (opt_section[0] == '\0') /* empty string */
        opt_section = DEFAULT_SECTION;

    vpm = X509_VERIFY_PARAM_new();
    if (vpm == NULL) {
        CMP_err("out of memory");
        goto err;
    }

    /* read default values for options from config file */
    configfile = opt_config != NULL ? opt_config : default_config_file;
    if (configfile && configfile[0] != '\0' /* non-empty string */
            && (configfile != default_config_file
                    || access(configfile, F_OK) != -1)) {
        CMP_info1("using OpenSSL configuration file '%s'", configfile);
        conf = app_load_config(configfile);
        if (conf == NULL) {
            goto err;
        } else {
            if (strcmp(opt_section, CMP_SECTION) == 0) { /* default */
                if (!NCONF_get_section(conf, opt_section))
                    CMP_info2("no [%s] section found in config file '%s';"
                              " will thus use just [default] and unnamed section if present",
                              opt_section, configfile);
            } else {
                const char *end = opt_section + strlen(opt_section);
                while ((end = prev_item(opt_section, end)) != NULL) {
                    if (!NCONF_get_section(conf, opt_item)) {
                        CMP_err2("no [%s] section found in config file '%s'",
                                 opt_item, configfile);
                        goto err;
                    }
                }
            }
            if (!read_config())
                goto err;
        }
    }
    (void)BIO_flush(bio_err); /* prevent interference with opt_help() */

    ret = get_opts(argc, argv);
    if (ret <= 0)
        goto err;
    ret = 0;

    if (opt_batch) {
#ifndef OPENSSL_NO_ENGINE
        UI_METHOD *ui_fallback_method;
# ifndef OPENSSL_NO_UI_CONSOLE
        ui_fallback_method = UI_OpenSSL();
# else
        ui_fallback_method = (UI_METHOD *)UI_null();
# endif
        UI_method_set_reader(ui_fallback_method, NULL);
#endif
    }

    if (opt_engine != NULL)
        e = setup_engine_methods(opt_engine, 0 /* not: ENGINE_METHOD_ALL */, 0);

    if (opt_port != NULL) {
        if (opt_use_mock_srv) {
            CMP_err("cannot use both -port and -use_mock_srv options");
            goto err;
        }
        if (opt_server != NULL) {
            CMP_err("cannot use both -port and -server options");
            goto err;
        }
    }

    if ((cmp_ctx = OSSL_CMP_CTX_new()) == NULL) {
        CMP_err("out of memory");
        goto err;
    }
    if (!OSSL_CMP_CTX_set_log_cb(cmp_ctx, print_to_bio_out)) {
        CMP_err1("cannot set up error reporting and logging for %s", prog);
        goto err;
    }
    if ((opt_use_mock_srv || opt_port != NULL)) {
        OSSL_CMP_SRV_CTX *srv_ctx;

        if ((srv_ctx = setup_srv_ctx(e)) == NULL)
            goto err;
        OSSL_CMP_CTX_set_transfer_cb_arg(cmp_ctx, srv_ctx);
        if (!OSSL_CMP_CTX_set_log_cb(OSSL_CMP_SRV_CTX_get0_cmp_ctx(srv_ctx),
                                     print_to_bio_out)) {
            CMP_err1("cannot set up error reporting and logging for %s", prog);
            goto err;
        }
    }


    if (opt_port != NULL) { /* act as very basic CMP HTTP server */
#ifdef OPENSSL_NO_SOCK
        BIO_printf(bio_err, "Cannot act as server - sockets not supported\n");
#else
        BIO *acbio;
        BIO *cbio = NULL;
        int msgs = 0;

        if ((acbio = http_server_init_bio(prog, opt_port)) == NULL)
            goto err;
        while (opt_max_msgs <= 0 || msgs < opt_max_msgs) {
            OSSL_CMP_MSG *req = NULL;
            OSSL_CMP_MSG *resp = NULL;

            ret = http_server_get_asn1_req(ASN1_ITEM_rptr(OSSL_CMP_MSG),
                                           (ASN1_VALUE **)&req, &cbio, acbio,
                                           prog, 0, 0);
            if (ret == 0)
                continue;
            if (ret++ == -1)
                break; /* fatal error */

            ret = 0;
            msgs++;
            if (req != NULL) {
                resp = OSSL_CMP_CTX_server_perform(cmp_ctx, req);
                OSSL_CMP_MSG_free(req);
                if (resp == NULL)
                    break; /* treated as fatal error */
                ret = http_server_send_asn1_resp(cbio, "application/pkixcmp",
                                                 ASN1_ITEM_rptr(OSSL_CMP_MSG),
                                                 (const ASN1_VALUE *)resp);
                OSSL_CMP_MSG_free(resp);
                if (!ret)
                    break; /* treated as fatal error */
            }
            BIO_free_all(cbio);
            cbio = NULL;
        }
        BIO_free_all(cbio);
        BIO_free_all(acbio);
#endif
        goto err;
    }
    /* else act as CMP client */

    if (opt_use_mock_srv) {
        if (opt_server != NULL) {
            CMP_err("cannot use both -use_mock_srv and -server options");
            goto err;
        }
        if (opt_proxy != NULL) {
            CMP_err("cannot use both -use_mock_srv and -proxy options");
            goto err;
        }
        opt_server = mock_server;
        opt_proxy = "API";
    } else {
        if (opt_server == NULL) {
            CMP_err("missing -server option");
            goto err;
        }
    }

    if (!setup_client_ctx(cmp_ctx, e)) {
        CMP_err("cannot set up CMP context");
        goto err;
    }
    for (i = 0; i < opt_repeat; i++) {
        /* everything is ready, now connect and perform the command! */
        switch (opt_cmd) {
        case CMP_IR:
            newcert = OSSL_CMP_exec_IR_ses(cmp_ctx);
            if (newcert == NULL)
                goto err;
            break;
        case CMP_KUR:
            newcert = OSSL_CMP_exec_KUR_ses(cmp_ctx);
            if (newcert == NULL)
                goto err;
            break;
        case CMP_CR:
            newcert = OSSL_CMP_exec_CR_ses(cmp_ctx);
            if (newcert == NULL)
                goto err;
            break;
        case CMP_P10CR:
            newcert = OSSL_CMP_exec_P10CR_ses(cmp_ctx);
            if (newcert == NULL)
                goto err;
            break;
        case CMP_RR:
            if (OSSL_CMP_exec_RR_ses(cmp_ctx) == NULL)
                goto err;
            break;
        case CMP_GENM:
            {
                STACK_OF(OSSL_CMP_ITAV) *itavs;

                if (opt_infotype != NID_undef) {
                    OSSL_CMP_ITAV *itav =
                        OSSL_CMP_ITAV_create(OBJ_nid2obj(opt_infotype), NULL);
                    if (itav == NULL)
                        goto err;
                    OSSL_CMP_CTX_push0_genm_ITAV(cmp_ctx, itav);
                }

                if ((itavs = OSSL_CMP_exec_GENM_ses(cmp_ctx)) == NULL)
                    goto err;
                print_itavs(itavs);
                sk_OSSL_CMP_ITAV_pop_free(itavs, OSSL_CMP_ITAV_free);
                break;
            }
        default:
            break;
        }

        {
            /* print PKIStatusInfo (this is in case there has been no error) */
            int status = OSSL_CMP_CTX_get_status(cmp_ctx);
            char *buf = app_malloc(OSSL_CMP_PKISI_BUFLEN, "PKIStatusInfo buf");
            const char *string =
                OSSL_CMP_CTX_snprint_PKIStatus(cmp_ctx, buf,
                                               OSSL_CMP_PKISI_BUFLEN);

            CMP_print(bio_err,
                      status == OSSL_CMP_PKISTATUS_accepted ? "info" :
                      status == OSSL_CMP_PKISTATUS_rejection ? "server error" :
                      status == OSSL_CMP_PKISTATUS_waiting ? "internal error"
                                                           : "warning",
                      "received from %s %s %s", opt_server,
                      string != NULL ? string : "<unknown PKIStatus>", "");
            OPENSSL_free(buf);
        }

        if (opt_cacertsout != NULL) {
            STACK_OF(X509) *certs = OSSL_CMP_CTX_get1_caPubs(cmp_ctx);

            if (sk_X509_num(certs) > 0
                    && save_certs(cmp_ctx, certs, opt_cacertsout, "CA") < 0) {
                sk_X509_pop_free(certs, X509_free);
                goto err;
            }
            sk_X509_pop_free(certs, X509_free);
        }

        if (opt_extracertsout != NULL) {
            STACK_OF(X509) *certs = OSSL_CMP_CTX_get1_extraCertsIn(cmp_ctx);
            if (sk_X509_num(certs) > 0
                    && save_certs(cmp_ctx, certs, opt_extracertsout,
                                  "extra") < 0) {
                sk_X509_pop_free(certs, X509_free);
                goto err;
            }
            sk_X509_pop_free(certs, X509_free);
        }

        if (opt_certout != NULL && newcert != NULL) {
            STACK_OF(X509) *certs = sk_X509_new_null();

            if (certs == NULL || !sk_X509_push(certs, newcert)
                    || save_certs(cmp_ctx, certs, opt_certout,
                                  "enrolled") < 0) {
                sk_X509_free(certs);
                goto err;
            }
            sk_X509_free(certs);
        }
        if (!OSSL_CMP_CTX_reinit(cmp_ctx))
            goto err;
    }
    ret = 1;

 err:
    /* in case we ended up here on error without proper cleaning */
    cleanse(opt_keypass);
    cleanse(opt_newkeypass);
    cleanse(opt_otherpass);
    cleanse(opt_tls_keypass);
    cleanse(opt_secret);
    cleanse(opt_srv_keypass);
    cleanse(opt_srv_secret);

    if (ret != 1)
        OSSL_CMP_CTX_print_errors(cmp_ctx);

    ossl_cmp_mock_srv_free(OSSL_CMP_CTX_get_transfer_cb_arg(cmp_ctx));
    {
        APP_HTTP_TLS_INFO *http_tls_info =
            OSSL_CMP_CTX_get_http_cb_arg(cmp_ctx);

        if (http_tls_info != NULL) {
            SSL_CTX_free(http_tls_info->ssl_ctx);
            OPENSSL_free(http_tls_info);
        }
    }
    X509_STORE_free(OSSL_CMP_CTX_get_certConf_cb_arg(cmp_ctx));
    OSSL_CMP_CTX_free(cmp_ctx);
    X509_VERIFY_PARAM_free(vpm);
    release_engine(e);

    NCONF_free(conf); /* must not do as long as opt_... variables are used */
    OSSL_CMP_log_close();

    return ret == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
