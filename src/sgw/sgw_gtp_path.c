#define TRACE_MODULE _sgw_path
#include "core_debug.h"
#include "core_pkbuf.h"
#include "core_net.h"

#include "types.h"
#include "gtp_types.h"
#include "gtp_path.h"

#include "sgw_context.h"
#include "sgw_event.h"
#include "sgw_gtp_path.h"

static int _gtpv2_c_recv_cb(net_sock_t *sock, void *data)
{
    event_t e;
    status_t rv;
    pkbuf_t *pkbuf = NULL;
    c_uint32_t addr;
    c_uint16_t port;
    event_e event = (event_e)data;

    d_assert(sock, return -1, "Null param");
    d_assert(data, return -1, "Null param");

    pkbuf = gtp_read(sock);
    if (pkbuf == NULL)
    {
        if (sock->sndrcv_errno == EAGAIN)
            return 0;

        return -1;
    }

    addr = sock->remote.sin_addr.s_addr;
    port = ntohs(sock->remote.sin_port);

    event_set(&e, event);
    event_set_param2(&e, (c_uintptr_t)pkbuf);
    if (event == SGW_EVT_S11_MESSAGE)
    {
        sgw_mme_t *mme = sgw_mme_find(addr, port);
        if (!mme)
        {
            mme = sgw_mme_add();
            d_assert(mme, return -1, "Can't add MME-GTP node");

            mme->addr = addr;
            mme->port = port;
            mme->sock = sock;
        }

        d_trace(10, "S11 PDU received from MME\n");

        event_set_param1(&e, (c_uintptr_t)mme);
    }
    else if (event == SGW_EVT_S5C_MESSAGE)
    {
        sgw_pgw_t *pgw = sgw_pgw_find(addr, port);
        d_assert(pgw, return -1, "Can't add PGW-GTP node");

        d_trace(10, "S5-C PDU received from PGW\n");

        event_set_param1(&e, (c_uintptr_t)pgw);
    }
    else
        d_assert(0, pkbuf_free(pkbuf); return -1, "Unknown GTP-Node");

    d_trace_hex(10, pkbuf->payload, pkbuf->len);

    rv = sgw_event_send(&e);
    if (rv != CORE_OK)
    {
        d_error("sgw_event_send error");
        pkbuf_free(pkbuf);
        return -1;
    }

    return 0;
}

static int _gtpv1_s5u_recv_cb(net_sock_t *sock, void *data)
{
    pkbuf_t *pkbuf = NULL;
    gtp_node_t gnode;
    gtp_header_t *gtp_h = NULL;
    sgw_bearer_t *bearer = NULL;
    c_uint32_t teid;

    d_assert(sock, return -1, "Null param");

    pkbuf = gtp_read(sock);
    if (pkbuf == NULL)
    {
        if (sock->sndrcv_errno == EAGAIN)
            return 0;

        return -1;
    }

    d_trace(50, "S5-U PDU received from PGW\n");
    d_trace_hex(50, pkbuf->payload, pkbuf->len);

    gtp_h = (gtp_header_t *)pkbuf->payload;
    if (gtp_h->type == GTPU_MSGTYPE_ECHO_REQ)
    {
        pkbuf_t *echo_rsp;

        d_trace(3, "Received echo-req");
        echo_rsp = gtp_handle_echo_req(pkbuf);
        if (echo_rsp)
        {
            /* Echo reply */
            d_trace(3, "Send echo-rsp to peer(PGW) ");

            gnode.addr = sock->remote.sin_addr.s_addr;
            gnode.port = GTPV1_U_UDP_PORT;
            gnode.sock = sgw_self()->s5u_sock;

            gtp_send(&gnode, echo_rsp);
            pkbuf_free(echo_rsp);
        }
    }
    else if (gtp_h->type == GTPU_MSGTYPE_GPDU)
    {
        teid = ntohl(gtp_h->teid);

        d_trace(50, "Recv GPDU (teid = 0x%x)",teid);

        bearer = sgw_bearer_find_by_sgw_s5u_teid(teid);
        if (bearer)
        {
            if (bearer->enb_s1u_teid)
            {
                /* Convert Teid and send to enodeB  via s1u */
                gtp_h->teid =  htonl(bearer->enb_s1u_teid);
                
                gnode.addr = bearer->enb_s1u_addr;
                gnode.port = GTPV1_U_UDP_PORT;
                gnode.sock = sgw_self()->s1u_sock;

                /* If there is buffered packet, send it first */
                if (bearer->num_buffered_pkt)
                {
                    int i;
                    for (i = 0; i < bearer->num_buffered_pkt; i++)
                    {
                        gtp_h = 
                            (gtp_header_t *)bearer->buffered_pkts[i]->payload;
                        gtp_h->teid =  htonl(bearer->enb_s1u_teid);

                        gtp_send(&gnode, bearer->buffered_pkts[i]);
                        pkbuf_free(bearer->buffered_pkts[i]);
                    }
                    bearer->num_buffered_pkt = 0;
                }

                gtp_send(&gnode, pkbuf);
            }
            else
            {
                /* S1U path is deactivated.
                 * Send downlink_data_notification to MME.
                 *
                 */
                sgw_ue_t *sgw_ue = NULL;

                d_assert(bearer->sess, pkbuf_free(pkbuf); return 0,
                        "Session is NULL");
                d_assert(bearer->sess->sgw_ue, pkbuf_free(pkbuf); return 0,
                        "SGW_UE  is NULL");

                sgw_ue = bearer->sess->sgw_ue;

                if ((SGW_GET_UE_STATE(sgw_ue) & SGW_S1U_INACTIVE))
                {
                    if ( !(SGW_GET_UE_STATE(sgw_ue) & SGW_DL_NOTI_SENT))
                    {
                        event_t e;
                        status_t rv;

                        event_set(&e, SGW_EVT_LO_DLDATA_NOTI);
                        event_set_param1(&e, (c_uintptr_t)bearer->index);
                        rv = sgw_event_send(&e);
                        if (rv != CORE_OK)
                        {
                            d_error("sgw_event_send error");
                            pkbuf_free(pkbuf);
                            return -1;
                        }

                        SGW_SET_UE_STATE(sgw_ue, SGW_DL_NOTI_SENT);
                    }

                    /* Buffer the packet */
                    if (bearer->num_buffered_pkt < MAX_NUM_BUFFER_PKT)
                    {
                        bearer->buffered_pkts[bearer->num_buffered_pkt++] = 
                            pkbuf;
                        return 0;
                    }
                }
                else
                {
                    /* UE is S1U_ACTIVE state but there is no s1u teid */
                    d_warn("UE is ACITVE but there is no matched "
                            "s1u_teid(tedid = 0x%x)",teid);

                    /* Just drop it */
                }

            }
        }
    }

    pkbuf_free(pkbuf);
    return 0;
}

static int _gtpv1_s1u_recv_cb(net_sock_t *sock, void *data)
{
    pkbuf_t *pkbuf = NULL;
    gtp_node_t gnode;
    gtp_header_t *gtp_h = NULL;
    sgw_tunnel_t *tunnel = NULL;
    c_uint32_t teid;

    d_assert(sock, return -1, "Null param");

    pkbuf = gtp_read(sock);
    if (pkbuf == NULL)
    {
        if (sock->sndrcv_errno == EAGAIN)
            return 0;

        return -1;
    }

    d_trace(50, "S1-U PDU received from ENB\n");
    d_trace_hex(50, pkbuf->payload, pkbuf->len);

    gtp_h = (gtp_header_t *)pkbuf->payload;
    if (gtp_h->type == GTPU_MSGTYPE_ECHO_REQ)
    {
        pkbuf_t *echo_rsp;

        d_trace(3, "Received echo-req\n");
        echo_rsp = gtp_handle_echo_req(pkbuf);
        if (echo_rsp)
        {
            /* Echo reply */
            d_trace(3, "Send echo-rsp to peer(ENB)\n");

            gnode.addr = sock->remote.sin_addr.s_addr;
            gnode.port = GTPV1_U_UDP_PORT;
            gnode.sock = sgw_self()->s1u_sock;

            gtp_send(&gnode, echo_rsp);
            pkbuf_free(echo_rsp);
        }
    }
    else if (gtp_h->type == GTPU_MSGTYPE_GPDU)
    {
        teid = ntohl(gtp_h->teid);
        d_trace(50, "Recv GPDU (teid = 0x%x) from ENB\n",teid);

        tunnel = sgw_tunnel_find_by_teid(teid);
        if (tunnel)
        {
            gtp_h->teid =  htonl(tunnel->remote_teid);
            
            gnode.addr = tunnel->remote_addr;
            gnode.port = GTPV1_U_UDP_PORT;
            if (tunnel->interface_type == GTP_F_TEID_S1_U_ENODEB_GTP_U)
                gnode.sock = sgw_self()->s5u_sock;
            else if (tunnel->interface_type ==
                    GTP_F_TEID_ENODEB_GTP_U_FOR_DL_DATA_FORWARDING)
                gnode.sock = sgw_self()->s1u_sock;
            else if (tunnel->interface_type ==
                    GTP_F_TEID_ENODEB_GTP_U_FOR_UL_DATA_FORWARDING)
                gnode.sock = sgw_self()->s1u_sock;
            else
                d_assert(0, return -1, "Invalid type(%d)",
                        tunnel->interface_type);

            gtp_send(&gnode, pkbuf);
        }
#if 0
        bearer = sgw_bearer_find_by_sgw_s1u_teid(teid);
        if (bearer)
        {
            /* Convert Teid and send to PGW  via s5u */
            gtp_h->teid =  htonl(bearer->pgw_s5u_teid);
            
            gnode.addr = bearer->pgw_s5u_addr;
            gnode.port = GTPV1_U_UDP_PORT;
            gnode.sock = sgw_self()->s5u_sock;

            gtp_send(&gnode, pkbuf);
        }
#endif
    }

    pkbuf_free(pkbuf);
    return 0;
}

status_t sgw_gtp_open()
{
    status_t rv;

    rv = gtp_listen(&sgw_self()->s11_sock, _gtpv2_c_recv_cb, 
            sgw_self()->s11_addr, sgw_self()->s11_port,
            (void*)SGW_EVT_S11_MESSAGE);
    if (rv != CORE_OK)
    {
        d_error("Can't establish S11 Path for SGW");
        return rv;
    }

    rv = gtp_listen(&sgw_self()->s5c_sock, _gtpv2_c_recv_cb, 
            sgw_self()->s5c_addr, sgw_self()->s5c_port,
            (void*)SGW_EVT_S5C_MESSAGE);
    if (rv != CORE_OK)
    {
        d_error("Can't establish S5-C Path for SGW");
        return rv;
    }

    rv = gtp_listen(&sgw_self()->s5u_sock, _gtpv1_s5u_recv_cb, 
            sgw_self()->s5u_addr, sgw_self()->s5u_port, NULL);
    if (rv != CORE_OK)
    {
        d_error("Can't establish S5-U Path for SGW");
        return rv;
    }

    rv = gtp_listen(&sgw_self()->s1u_sock, _gtpv1_s1u_recv_cb, 
            sgw_self()->s1u_addr, sgw_self()->s1u_port, NULL);
    if (rv != CORE_OK)
    {
        d_error("Can't establish S1-U Path for SGW");
        return rv;
    }

    return CORE_OK;
}

status_t sgw_gtp_close()
{
    status_t rv;

    rv = gtp_close(sgw_self()->s11_sock);
    if (rv != CORE_OK)
    {
        d_error("Can't close S11 Path for MME");
        return rv;
    }

    rv = gtp_close(sgw_self()->s5c_sock);
    if (rv != CORE_OK)
    {
        d_error("Can't close S5-C Path for MME");
        return rv;
    }

    rv = gtp_close(sgw_self()->s5u_sock);
    if (rv != CORE_OK)
    {
        d_error("Can't close S5-U Path for MME");
        return rv;
    }

    return CORE_OK;
}

status_t sgw_gtp_send_end_marker(sgw_bearer_t *bearer)
{
    status_t rv;
    pkbuf_t *pkbuf = NULL;
    gtp_header_t *h = NULL;
    gtp_node_t gnode;

    d_assert(bearer, return CORE_ERROR,);

    pkbuf = pkbuf_alloc(0, 100 /* enough for END_MARKER; use smaller buffer */);
    d_assert(pkbuf, return CORE_ERROR,);
    h = (gtp_header_t *)pkbuf->payload;

    memset(h, 0, GTPV1U_HEADER_LEN);

    /*
     * Flags
     * 0x20 - Version : GTP release 99 version (1)
     * 0x10 - Protocol Type : GTP (1)
     */
    h->flags = 0x30;
    h->type = GTPU_MSGTYPE_END_MARKER;
    h->teid =  htonl(bearer->enb_s1u_teid);
    
    gnode.addr = bearer->enb_s1u_addr;
    gnode.port = GTPV1_U_UDP_PORT;
    gnode.sock = sgw_self()->s1u_sock;

    rv = gtp_send(&gnode, pkbuf);
    d_assert(rv == CORE_OK, , "gtp send failed");
    pkbuf_free(pkbuf);

    return rv;
}
