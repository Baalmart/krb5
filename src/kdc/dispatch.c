/*
 * $Source$
 * $Author$
 *
 * Copyright 1990 by the Massachusetts Institute of Technology.
 *
 * For copying and distribution information, please see the file
 * <krb5/copyright.h>.
 *
 * Dispatch an incoming packet.
 */

#if !defined(lint) && !defined(SABER)
static char rcsid_dispatch_c[] =
"$Id$";
#endif	/* !lint & !SABER */

#include <krb5/copyright.h>
#include <krb5/krb5.h>
#include <krb5/asn1.h>
#include <krb5/kdb.h>
#include <syslog.h>
#include "kdc_util.h"

krb5_error_code
dispatch(pkt, from, response)
krb5_data *pkt;
const krb5_fulladdr *from;
krb5_data **response;
{

    krb5_error_code retval;
    krb5_kdc_req *as_req;
    krb5_kdc_req *tgs_req;

    /* decode incoming packet, and dispatch */

    /* try the replay lookaside buffer */
    if (kdc_check_lookaside(pkt, response)) {
	/* a hit! */
	syslog(LOG_INFO, "DISPATCH: replay found and re-transmitted");
	return 0;
    }
    /* try TGS_REQ first; they are more common! */

    if (krb5_is_tgs_req(pkt)) {
	if (!(retval = decode_krb5_tgs_req(pkt, &tgs_req))) {
	    retval = process_tgs_req(tgs_req, from, response);
	    krb5_free_kdc_req(tgs_req);
	}
    } else if (krb5_is_as_req(pkt)) {
	if (!(retval = decode_krb5_as_req(pkt, &as_req))) {
	    retval = process_as_req(as_req, from, response);
	    krb5_free_kdc_req(as_req);
	}
    }
#ifdef KRB4
    else if (pkt->data[0] == 4)		/* old version */
	retval = process_v4(pkt, from, response);
#endif
    else
	retval = KRB5KRB_AP_ERR_MSG_TYPE;
    /* put the response into the lookaside buffer */
    if (!retval)
	kdc_insert_lookaside(pkt, *response);

    return retval;
}
