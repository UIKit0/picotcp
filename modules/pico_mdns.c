/*********************************************************************
 PicoTCP. Copyright (c) 2014 TASS Belgium NV. Some rights reserved.
 See LICENSE and COPYING for usage.
 .
 Author: Toon Stegen
 *********************************************************************/

/* picoTCP */
#include "pico_config.h"
#include "pico_stack.h"
#include "pico_addressing.h"
#include "pico_socket.h"
#include "pico_ipv4.h"
#include "pico_ipv6.h"
#include "pico_mdns.h"
#include "pico_tree.h"

#ifdef PICO_SUPPORT_MDNS

/* Debugging */
//#define mdns_dbg(...) do {} while(0)
#define mdns_dbg dbg

#define PICO_MDNS_QUERY_TIMEOUT (10000) /* Ten seconds */
#define PICO_MDNS_RR_TTL_TICK (1000)    /* One second */

/* mDNS MTU size */
#define PICO_MDNS_MTU 1400u

/* Constant strings */
#define PICO_ARPA_IPV4_SUFFIX ".in-addr.arpa"
#define PICO_ARPA_IPV6_SUFFIX ".IP6.ARPA"
#define PICO_OK_STRING ((char *)"OK")
#define PICO_NOK_STRING ((char *)"NOK")
#define PICO_SOK_STRING ((char *)"SOK")

/* Question flags */
#define PICO_MDNS_QUESTION_FLAG_PROBE 0x01u
#define PICO_MDNS_QUESTION_FLAG_NO_PROBE 0x00u
#define PICO_MDNS_QUESTION_FLAG_UNICAST_RES 0x02u
#define PICO_MDNS_QUESTION_FLAG_MULTICAST_RES 0x00u

#define IS_QUESTION_PROBE_FLAG_SET(x) (((x) & PICO_MDNS_QUESTION_FLAG_UNICAST_RES) ? 1 : 0 )
#define IS_QUESTION_UNICAST_FLAG_SET(x) (((x) & PICO_MDNS_QUESTION_FLAG_PROBE) ? 1 : 0 )
#define IS_QUESTION_MULTICAST_FLAG_SET(x) (((x) & PICO_MDNS_QUESTION_FLAG_PROBE) ? 0 : 1 )


/* Resource Record flags */
#define PICO_MDNS_RES_RECORD_PROBED 0x40u
#define PICO_MDNS_RES_RECORD_CLAIMED 0x80u

#define IS_RES_RECORD_FLAG_CLAIM_SHARED_SET(x) (((x) & PICO_MDNS_RES_RECORD_SHARED) ? 1 : 0)
#define IS_RES_RECORD_FLAG_CLAIM_UNIQUE_SET(x) (((x) & PICO_MDNS_RES_RECORD_SHARED) ? 0 : 1)
#define IS_RES_RECORD_FLAG_PROBED_SET(x) (((x) & PICO_MDNS_RES_RECORD_PROBED) ? 1 : 0)
#define IS_RES_RECORD_FLAG_CLAIMED_SET(x) (((x) & PICO_MDNS_RES_RECORD_CLAIMED) ? 1 : 0)

/* Set and Clear MSB of BE short */
#define PICO_MDNS_SET_MSB_BE(x) (x = x | (uint16_t)(0x0080u))
#define PICO_MDNS_CLR_MSB_BE(x) (x = x & (uint16_t)(0xff7fu))

#define PICO_MDNS_SET_FLAG(x, b) (x = ((x) | (uint8_t)(b)))
#define PICO_MDNS_CLR_FLAG(x, b) (x = ((x) & (~((uint8_t)(b)))

static struct pico_ip4 inaddr_any = { 0 };

/* Global socket and port for all mdns communication */
static struct pico_socket *mdns_sock_ipv4 = NULL;

/* RFC:
 *  fully compliant mDNS Querier MUST send its Multicast DNS queries from
 *  UDP source port 5353, and MUST listen for Multicast DNS replies sent
 *  to UDP destination port 5353 at the mDNS link-local multicast address
 *  (224.0.0.251 and/or its IPv6 equivalent FF02::FB)
 */
static uint16_t mdns_port = 5353u;

/* struct containing status of a query */
struct pico_mdns_cookie
{
    pico_dns_packet *packet;            // Pointer to DNS packet
    char *qname;                        // Hostname being queried
    uint16_t qtype;                     // qtype
    uint16_t qclass;                    // qclass
    uint8_t count;                      // Number of packets to send
    uint8_t flags;                      // Flags: ... | uni/multi | probe |
    uint16_t len;                       // Length of packet
    struct pico_timer *timer;           // For Timer events
    void (*callback)(char *, void *);   // MARK: Callback
    void *arg;                          // Argument to pass to callback
};

/* struct containing the information about a cache record */
struct pico_mdns_cache_rr
{
    char *url;                              // Hostname
    struct pico_dns_res_record_suffix *suf; // Type, Class, TTL and rdata length
    char *rdata;                            // Resource Record Data
    struct pico_timer *timer;               // For Timer events
};

/* **************************************************************************
 * Compare-function for two cache entries to give to the tree
 * **************************************************************************/
static int mdns_cache_cmp(void *ka, void *kb)
{
    struct pico_mdns_cache_rr *a = ka, *b = kb;
    uint32_t ha = 0, hb = 0;
    
    /* Cache is sorted by qtype, name */
    if(a->suf->rtype < b->suf->rtype)
        return -1;
    if(b->suf->rtype < a->suf->rtype)
        return 1;
    
    /* Hash strings to compare */
    ha = pico_hash(a->url, (uint32_t)strlen(a->url));
    hb = pico_hash(b->url, (uint32_t)strlen(b->url));
    
    if(ha < hb)
        return -1;
    if(hb < ha)
        return 1;
    
    return 0;
}

/* **************************************************************************
 * Compare-function for two query-cookies to give to the tree
 * **************************************************************************/
static int mdns_cmp(void *ka, void *kb)
{
    struct pico_mdns_cookie *a = ka, *b = kb;
    uint32_t ha = 0, hb = 0;
    
    /* Cookie is sorted by qtype, name */
    if(a->qtype < b->qtype)
        return -1;
    if(b->qtype < a->qtype)
        return 1;
    
    /* Hash strings to compare */
    ha = pico_hash(a->qname, (uint32_t)strlen(a->qname));
    hb = pico_hash(b->qname, (uint32_t)strlen(b->qname));
    
    if(ha < hb)
        return -1;
    if(hb < ha)
        return 1;
    
    return 0;
}

/* **************************************************************************
 * Compare-function for two mDNS resource records to give to a tree
 * **************************************************************************/
static int mdns_res_record_cmp( void *rra, void *rrb )
{
    struct pico_mdns_res_record *a = (struct pico_mdns_res_record *)rra;
    struct pico_mdns_res_record *b = (struct pico_mdns_res_record *)rrb;
    uint32_t ha = 0, hb = 0;
    
    /* Lexicraphically qtype first */
    if (a->record->rsuffix->rtype < b->record->rsuffix->rtype)
        return -1;
    if (a->record->rsuffix->rtype > b->record->rsuffix->rtype)
        return 1;
    
    /* Then hash strings to compare */
    ha = pico_hash(a->record->rname, (uint32_t)strlen(a->record->rname));
    hb = pico_hash(b->record->rname, (uint32_t)strlen(b->record->rname));
    
    /* Compare hashes */
    if (ha < hb)
        return -1;
    if (hb < ha)
        return 1;
    
    return 0;
}

/* Cache records for the mDNS hosts in the network */
PICO_TREE_DECLARE(CacheTable, mdns_cache_cmp);

/* Tree containing query-cookies */
PICO_TREE_DECLARE(QTable, mdns_cmp);

/* Tree contain records I want to claim */
PICO_TREE_DECLARE(MyRecordsTable, mdns_res_record_cmp);

// MARK: MDNS PACKET UTILITIES

/* Sends an mdns packet on the global socket*/
static int pico_mdns_send_packet(pico_dns_packet *packet, uint16_t len)
{
    struct pico_ip4 dst4;
    
    /* Send packet to IPv4 socket */
    pico_string_to_ipv4(PICO_MDNS_DEST_ADDR4, &dst4.addr);
    return pico_socket_sendto(mdns_sock_ipv4, packet, (int)len, &dst4, short_be(mdns_port));
}

#ifdef PICO_SUPPORT_IPV6
static struct pico_ip6 *pico_get_ip6_from_ip4(struct pico_ip4 *ipv4_addr)
{
    struct pico_device *dev = NULL;
    struct pico_ipv6_link *link = NULL;
    if((dev = pico_ipv4_link_find(ipv4_addr)) == NULL) {
        mdns_dbg("Could not find device!\n");
        return NULL;
    }
    
    if((link = pico_ipv6_link_by_dev(dev)) == NULL) {
        mdns_dbg("Could not find link!\n");
        return NULL;
    }
    
    return &link->address;
}
#endif

// MARK: QUESTION UTILITIES

static struct pico_dns_question *pico_mdns_question_create( const char *url, uint16_t *len, uint8_t proto, uint16_t qtype, uint8_t flags )
{
    uint16_t _qtype = 0;                            // qtype
    uint16_t qclass = short_be(PICO_DNS_CLASS_IN);  // qclass
    
    /* Set the MSB of the qclass field according to the mDNS format */
    if (IS_QUESTION_UNICAST_FLAG_SET(flags))
        PICO_MDNS_SET_MSB_BE(qclass);
    else
        PICO_MDNS_CLR_MSB_BE(qclass);
    
    /* Make the class LE again */
    qclass = short_be(qclass);
    
    /* Fill in the question suffix */
    if (IS_QUESTION_PROBE_FLAG_SET(flags)) {
        /* RFC:
         *  All probe queries SHOULD be done using the desired resource
         *  record name and class (usually class 1, "Internet"), and
         *  query type "ANY" (255), to elicit answers for all
         *  types of records with that name.
         */
        _qtype = PICO_DNS_TYPE_ANY;
    } else {
        _qtype = qtype;
    }
    
    /* Create a question as you would with plain DNS */
    return pico_dns_question_create(url, len, proto, _qtype, qclass);
}

// MARK: MDNS QUERY UTILITIES

/* **************************************************************************
 *  Creates a DNS packet meant for querying. Currently only questions can be
 *  inserted in the packet.
 *
 *  TODO: Allow resource records to be added to Authority & Answer Section
 *          - Answer Section: To implement Known-Answer Suppression
 *          - Authority Section: To implement probe queries and tiebreaking
 * **************************************************************************/
static pico_dns_packet *pico_mdns_query_create( struct pico_dns_question *question_list, struct pico_dns_res_record *answer_list, struct pico_dns_res_record *authority_list, struct pico_dns_res_record *additional_list, uint16_t *len )
{
    pico_dns_packet *packet = NULL;
    
    /* Create an answer as you would with plain DNS */
    packet = pico_dns_query_create(question_list, answer_list, authority_list, additional_list, len);
    if (!packet) {
        mdns_dbg("Could not create DNS query!\n");
        return NULL;
    }
    
    /* Set the id of the DNS packet to 0 */
    packet->id = 0;
    
    return packet;
}

// MARK: RRECORD UTILITIES

/* **************************************************************************
 *  Create a resource record for the mDNS resource record format, that is
 *  with the MSB of the rclass field being set accordingly.
 * **************************************************************************/
static struct pico_dns_res_record *pico_mdns_rr_create( const char *url,
                                                        void *_rdata,
                                                        uint16_t *len,
                                                        uint16_t rtype,
                                                        uint16_t rttl,
                                                        uint8_t flags )
{
    uint16_t rclass = short_be(PICO_DNS_CLASS_IN);  // rclass
    
    /* Set the MSB of the rclass field according to the mDNS format */
    if (IS_RES_RECORD_FLAG_CLAIM_UNIQUE_SET(flags))
        PICO_MDNS_SET_MSB_BE(rclass);
    else
        PICO_MDNS_CLR_MSB_BE(rclass);
    
    /* Make the class LE again */
    rclass = short_be(rclass);
    
    /* Create a resource record as you would with plain DNS */
    return pico_dns_rr_create(url, _rdata, len, rtype, rclass, rttl);
}

/* **************************************************************************
 *  Creates a new mDNS resource record for which you want to have the
 *  authority, and adds it to the end of the [records] list. If a NULL-
 *  pointer is provided a new list will be created. Fills up len with the
 *  size of the DNS record
 * **************************************************************************/
int pico_mdns_res_record_create( pico_mdns_res_record_list **records,
                                const char *url,
                                void *_rdata,
                                uint16_t rtype,
                                uint16_t rttl,
                                uint8_t flags )
{
    struct pico_mdns_res_record **iterator = NULL;  // Iterator for the list
    uint16_t len = 0;
    
    /* Put iterator at beginning of list */
    iterator = records;
    
    /* Move to the end of the list */
    while (*iterator) {
        (*iterator) = (*iterator)->next;
    }
    
    /* Provide space for the new mDNS resource record */
    *iterator = PICO_ZALLOC(sizeof(struct pico_mdns_res_record));
    if (!(*iterator)) {
        mdns_dbg("Could not provide space for the mDNS resource record");
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    /* Create a new record at the end of the lisst */
    (*iterator)->record = pico_mdns_rr_create(url, _rdata, &len, rtype, rttl, flags);
    
    if (!(*iterator)->record) {
        mdns_dbg("Creating mDNS resource record failed!\n");
        return -1;
    }
    
    /* Initialise fields */
    (*iterator)->timer = NULL;
    (*iterator)->next = NULL;
    (*iterator)->flags = flags;
    
    return 0;
}

// MARK: ANSWER UTILITIES

/* **************************************************************************
 *  Create a resource record for the mDNS answer message format, that is
 *  with the identifier of the DNS packet being 0.
 * **************************************************************************/
static pico_dns_packet *pico_mdns_answer_create( struct pico_dns_res_record *answer_list, struct pico_dns_res_record *authority_list, struct pico_dns_res_record *additional_list, uint16_t *len )
{
    pico_dns_packet *packet = NULL;
    
    /* Create an answer as you would with plain DNS */
    packet = pico_dns_answer_create(answer_list, authority_list, additional_list, len);
    if (!packet) {
        mdns_dbg("Could not create DNS answer!\n");
        return NULL;
    }
    
    /* Set the id of the DNS packet to 0 */
    packet->id = 0;
    
    return packet;
}

// MARK: CACHE UTILITIES

static int pico_mdns_cache_del_rr(char *url, uint16_t qtype, char *rdata)
{
    struct pico_mdns_cache_rr test, *found = NULL;
    
    test.suf = PICO_ZALLOC(sizeof(struct pico_dns_res_record_suffix));
    if(!test.suf)
        return -1;
    
    test.url = url;
    test.suf->rclass = PICO_DNS_CLASS_IN; /* We only support IN */
    test.suf->rtype = qtype;
    test.rdata = rdata;
    
    found = pico_tree_findKey(&CacheTable, &test);
    PICO_FREE(test.suf);
    
    if(!found) {
        mdns_dbg("Couldn't find cache RR to delete\n");
        return -1;
    }
    
    mdns_dbg("Removing RR: qtype '%d' url '%s'\n", qtype, url);
    
    pico_tree_delete(&CacheTable, found);
    PICO_FREE(found->url);
    PICO_FREE(found->suf);
    PICO_FREE(found->rdata);
    PICO_FREE(found);
    return 0;
}

static void pico_mdns_cache_tick(pico_time now, void *_arg)
{
    struct pico_mdns_cache_rr *rr = (struct pico_mdns_cache_rr *)_arg;
    IGNORE_PARAMETER(now);
    
    rr->suf->rttl--;
    mdns_dbg("TTL UPDATE: '%s' - qtype: %d - TTL: %d\n", rr->url, rr->suf->rtype, rr->suf->rttl);
    if(rr->suf->rttl < 1)
        pico_mdns_cache_del_rr(rr->url, rr->suf->rtype, rr->rdata);
    else
        rr->timer = pico_timer_add(PICO_MDNS_RR_TTL_TICK, pico_mdns_cache_tick, rr);
    
    /* TODO: continuous querying: cache refresh at 80 or 85/90/95/100 % of TTL + 2% rnd */
}

/* Look for a RR in cache matching hostname and qtype */
static struct pico_mdns_cache_rr *pico_mdns_cache_find_rr(const char *url, uint16_t rtype)
{
    struct pico_mdns_cache_rr test;                 /* Create a test-rr for the tree_findKey-function */
    struct pico_mdns_cache_rr *found = NULL;        /* Resource Record pointer to return */
    struct pico_dns_res_record_suffix *suf = NULL;  /* Create a test rsuffix for the test-rr */
    
    /* Provide space for the test-suffix */
    suf = PICO_ZALLOC(sizeof(struct pico_dns_res_record_suffix));
    if(!suf)
        return NULL;
    
    /* Set the rsuffix pointer of the test-rr */
    test.suf = suf;
    
    /* Set the rtype of the rsuffix */
    test.suf->rtype = rtype;
    
    /* Set the url of the test-rr in DNS name format */
    test.url = pico_dns_url_to_qname(url);
    
    /* Find the Resource Record in the tree */
    found = pico_tree_findKey(&CacheTable, &test);
    
    /* Free allocated space */
    PICO_FREE(test.url);
    PICO_FREE(suf);
    
    return found;
}

static int pico_mdns_cache_add_rr(char *url, struct pico_dns_res_record_suffix *suf, char *rdata)
{
    struct pico_mdns_cache_rr *rr = NULL;
    struct pico_mdns_cache_rr *found = NULL;
    struct pico_dns_res_record_suffix *rr_suf = NULL;
    char *rr_url = NULL;
    char *rr_rdata = NULL;
    
    if(!url || !suf || !rdata)
        return -1;
    
    /* Don't cache PTR answers */
    if(short_be(suf->rtype) == PICO_DNS_TYPE_PTR ) {
        mdns_dbg("Not caching PTR answer\n");
        return 0;
    }
    
    /* Provide space for the cache RR */
    rr = PICO_ZALLOC(sizeof(struct pico_mdns_cache_rr));
    rr_suf = PICO_ZALLOC(sizeof(struct pico_dns_res_record_suffix));
    rr_url = pico_dns_url_to_qname(url);
    rr_rdata = PICO_ZALLOC(short_be(suf->rdlength));
    if(!rr || !rr_suf || !rr_url || !rr_rdata) {
        PICO_FREE(rr);
        PICO_FREE(rr_suf);
        PICO_FREE(rr_url);
        PICO_FREE(rr_rdata);
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }
    
    /* Set the rname of the cache rr */
    rr->url = rr_url;
    
    memcpy(rr_suf, suf, sizeof(struct pico_dns_res_record_suffix));
    rr->suf = rr_suf;
    rr->suf->rtype = short_be(rr->suf->rtype);
    rr->suf->rclass = short_be(rr->suf->rclass);
    rr->suf->rttl = long_be(suf->rttl);
    rr->suf->rdlength = short_be(suf->rdlength);
    memcpy(rr_rdata, rdata, rr->suf->rdlength);
    rr->rdata = rr_rdata;
    
    found = pico_mdns_cache_find_rr(url, rr->suf->rtype);
    if(found) {
        if(rr->suf->rttl > 0) {
            mdns_dbg("RR already in cache, updating TTL (was %ds now %ds)\n", found->suf->rttl, rr->suf->rttl);
            found->suf->rttl = rr->suf->rttl;
        }
        else {
            mdns_dbg("RR scheduled for deletion\n");
            found->suf->rttl = 1;  /* TTL 0 means delete from cache but we need to wait one second */
        }
    }
    else {
        if(rr->suf->rttl > 0) {
            pico_tree_insert(&CacheTable, rr);
            mdns_dbg("RR cached. Starting TTL counter, TICK TACK TICK TACK..\n");
            rr->timer = pico_timer_add(PICO_MDNS_RR_TTL_TICK, pico_mdns_cache_tick, rr);
            return 0;
        }
        else {
            mdns_dbg("RR not in cache but TTL = 0\n");
        }
    }
    PICO_FREE(rr->suf);
    PICO_FREE(rr->url);
    PICO_FREE(rr->rdata);
    PICO_FREE(rr);
    return 0;
}

/* **************************************************************************
 *  Looks for a certain query-cookie in the global cookie-tree given an [url]
 *  and [qtype]. Qclass does not apply since all we use is qclass 'IN' or 1,
 *  anyway.
 * **************************************************************************/
static struct pico_mdns_cookie *pico_mdns_find_cookie( char *qname, uint16_t qtype )
{
    struct pico_mdns_cookie test;           /* Create a test-cookie for the tree_findKey-function */
    struct pico_mdns_cookie *found = NULL;  /* Pointer to return */
    
    /* Set qname & qtype of the test-cookie*/
    test.qname = qname;
    test.qtype = qtype;
    
    /* Find the cookie in the tree */
    found = pico_tree_findKey(&QTable, &test);
    
    return found;
}

/* **************************************************************************
 *  Deletes for a certain query-cookie in the global cookie-tree given an [url]
 *  and [qtype]. Qclass does not apply since all we use is qclass 'IN' or 1,
 *  anyway.
 * **************************************************************************/
static int pico_mdns_del_cookie( char *qname, uint16_t qtype )
{
    /* First, find the cookie in the global tree */
    struct pico_mdns_cookie *found = pico_mdns_find_cookie(qname, qtype);
    if (!found) {
        mdns_dbg("Could not find cookie '%s' to delete\n", qname);
        return -1;
    }
    
    /* Delete and free memory for the cookie */
    pico_tree_delete(&QTable, found);
    PICO_FREE(found->packet);
    PICO_FREE(found);
    
    mdns_dbg("Cookie deleted succesfully!\n");
    
    return 0;
}

/* Callback for the timeout timer of a query cookie */
static void pico_mdns_timeout(pico_time now, void *_arg)
{
    struct pico_mdns_cookie *ck = NULL;
    
    IGNORE_PARAMETER(now);
    
    /* Cast the _arg pointer to a cookie */
    ck = (struct pico_mdns_cookie *)_arg;

    /* Call the callback */
    if(ck->callback)
        ck->callback(NULL, ck->arg);
    
    /* Delete the cookie */
    pico_mdns_del_cookie(ck->qname, ck->qtype);
    
    /* TODO: If the request was for a reconfirmation of a record, flush the corresponding record after the timeout */
}

/* **************************************************************************
 *
 * Creates and fills in a cookie with a certain query [dns_packet] and add it
 * to the global cookie-tree. [len] is the length of the DNS packet in bytes.
 * [flags] are the MDNS-flags given for a certain query, like probing-flag,
 * unicast-response-flags. [callback] is callback that gets called when events
 * happen, [arg] is and argument passed to the callback-function
 *
 * **************************************************************************/
static struct pico_mdns_cookie *pico_mdns_add_cookie( pico_dns_packet *dns_packet, uint16_t len, uint8_t flags, uint8_t count, void (*callback)(char *str, void *arg), void *arg )
{
    /* Query cookie structs */
    struct pico_mdns_cookie *ck = NULL;
    struct pico_mdns_cookie *found = NULL;
    
    /* Provide space for such a cookie */
    ck = PICO_ZALLOC(sizeof(struct pico_mdns_cookie));
    if (!ck)
        return NULL;
    
    /* Fill in the form */
    ck->packet = dns_packet;
    ck->len = len;
    ck->qname = (char *)dns_packet + sizeof(struct pico_dns_header);
    pico_to_lowercase(ck->qname);
    ck->qtype = short_be(*(uint16_t *)((char *)dns_packet + len - 4));
    ck->qclass =  short_be(*(uint16_t *)((char *)dns_packet + len - 2));
    ck->flags = flags;
    ck->count = count;
    ck->callback = callback;
    ck->arg = arg;
    
    /* Insert the cookie into a tree */
    found = pico_tree_insert(&QTable, ck);
    
    /* If cookie is already in tree */
    if (found) {
        pico_err = PICO_ERR_EAGAIN;
        PICO_FREE(ck);
        PICO_FREE(dns_packet);
        return NULL;
    }
    
    mdns_dbg("Cookie '%s' with qtype '%x' added to QTable\n", ck->qname, ck->qtype);
    
    /* RFC:
     *  When no response is received within ten seconds, then, even though
     *  its TTL may indicate that it is not yet due to expire, that record
     *  SHOULD be promptly flushed from cache.
     */
    if(IS_QUESTION_PROBE_FLAG_SET(flags))
        ck->timer = pico_timer_add(PICO_MDNS_QUERY_TIMEOUT, pico_mdns_timeout, ck);
    
    return ck;
}

// MARK: ASYNCHRONOUS MDNS RECEPTION

/* handle a single incoming answer */
static int pico_mdns_handle_answer(char *url, struct pico_dns_res_record_suffix *suf, char *data)
{
    struct pico_mdns_cookie *ck = NULL;     // Temporary storage of query cookie
    
    /* Remove cache flush bit if set MARK: But why? */
    suf->rclass &= short_be((uint16_t) ~0x8000);
    
    /* Print some context */
    mdns_dbg("Answer for record %s was received:\n", url);
    mdns_dbg("rrtype: %u, rrclass: %u, ttl: %lu, rdlen: %u\n", short_be(suf->rtype), short_be(suf->rclass), (unsigned long)long_be(suf->rttl), short_be(suf->rdlength));
    
    /* Add a resource record to cache */
    pico_mdns_cache_add_rr(url, suf, data);
    
    mdns_dbg("Searching for a corresponding query cookie for url: %s and qtype: %d...\n", url, short_be(suf->rtype));
    
    /* Check in the query tree whether a request was sent to elicit this answer */
    ck = pico_mdns_find_cookie(url, short_be(suf->rtype));
    if(!ck) {
        /* MARK: Of course no cookie will be found if the responder doesn't sent the same qtype with probes*/
        mdns_dbg("Found NO corresponding cookie!\n");
        return 0;
    }
    
    mdns_dbg("Found a corresponding cookie!\n");
    
    /* if we are probing, set probe to zero so the probe timer stops the next time it goes off */
    if (IS_QUESTION_PROBE_FLAG_SET(ck->flags)) {
        mdns_dbg("Probe set to zero\n");
        ck->flags &= PICO_MDNS_QUESTION_FLAG_NO_PROBE;
        return 0;
    }
    
    /* If this was aan API request return answer passed in callback */
    if(short_be(suf->rtype) == PICO_DNS_TYPE_A) {
        uint32_t rdata = long_from(data);
        char peer_addr[46];
        pico_ipv4_to_string(peer_addr, long_from(&rdata));
        ck->callback(peer_addr, ck->arg);
    }
    
#ifdef PICO_SUPPORT_IPV6
    else if(short_be(suf->rtype) == PICO_DNS_TYPE_AAAA) {
        uint8_t *rdata = (uint8_t *) data;
        char peer_addr[46];
        pico_ipv6_to_string(peer_addr, rdata);
        ck->callback(peer_addr, ck->arg);
    }
#endif
    else if(short_be(suf->rtype) == PICO_DNS_TYPE_PTR) {
        pico_dns_notation_to_name(data);
        ck->callback(data + 1, ck->arg);    /* +1 to discard the beginning dot */
    } else {
        mdns_dbg("Unrecognised record type\n");
        ck->callback(NULL, ck->arg);
    }
    
    /* Remove the timer from the cookie and delete it */
    pico_timer_cancel(ck->timer);
    pico_mdns_del_cookie(url, ck->qtype);
    
    return 0;
}

/* Create a query answer according to the qtype (ANY, A, AAAA or PTR) of a certain IP address */
//static struct pico_dns_header *pico_mdns_query_create_answer(union pico_address *local_addr, uint16_t qtype, uint16_t *len, char *name)
//{
//    // TODO: If type is ANY include all records corresponding to the name
//    // TODO: Include negative responses for records this hosts knows they don't exist
//
//    IGNORE_PARAMETER(local_addr);
//    IGNORE_PARAMETER(qtype);
//    IGNORE_PARAMETER(len);
//    IGNORE_PARAMETER(name);
//    
//    if(qtype == PICO_DNS_TYPE_A || qtype == PICO_DNS_TYPE_ANY) {
//        return pico_mdns_answer_create(mdns_global_host, len, PICO_DNS_TYPE_A, local_addr);
//    }
//#ifdef PICO_SUPPORT_IPV6
//    if(qtype == PICO_DNS_TYPE_AAAA || qtype == PICO_DNS_TYPE_ANY) {
//        struct pico_ip6 *ip6 = pico_get_ip6_from_ip4(&local_addr->ip4);
//        return pico_mdns_answer_create(mdns_global_host, len, PICO_DNS_TYPE_AAAA, ip6);
//    }
//#endif
//    /* reply to PTR records */
//    if(qtype == PICO_DNS_TYPE_PTR) {
//        char host_conv[255] = { 0 };
//        mdns_dbg("Replying on PTR query...\n");
//        strcpy(host_conv + 1, mdns_global_host);
//        pico_dns_name_to_dns_notation(host_conv);
//        return pico_mdns_answer_create(name, len, qtype, host_conv);
//    }
//    
//    mdns_dbg("Unknown qtype!\n");
//
//    return NULL;
//}

/* Reply on a single query */
static int pico_mdns_reply_query(uint16_t qtype, struct pico_ip4 peer, char *name)
{
    IGNORE_PARAMETER(qtype);
    IGNORE_PARAMETER(peer);
    IGNORE_PARAMETER(name);
    
//    /* Pointer to DNS packet */
//    struct pico_dns_header *header = NULL;
//    
//    /* To store either an IPv4 or an IPv6 */
//    union pico_address *local_addr = NULL;
//    
//    uint16_t len; // Temporary storage of the length of the reply
//    
//    // TODO: Check for authority sections / probing queries
//    // TODO: Check for unicast response bit
//    
//    /* RFC:
//     *  If a responder receives a query addressed to the mDNS IPv4 link-local multicast address,
//     *  from a source address not apparently on the same subnet as the
//     *  responder, then, even if the query indicates that a unicast
//     *  response is preferred, the responder SHOULD elect to respond by multicast
//     *  anyway, since it can reasonably predict that a unicast response with
//     *  an apparently non-local source address will probably be ignored.
//     */
//    local_addr = (union pico_address *) pico_ipv4_source_find(&peer);
//    if (!local_addr) {
//        // TODO: Forced Response via multicast
//        pico_err = PICO_ERR_EHOSTUNREACH;
//        mdns_dbg("Peer not on same subnet!\n");
//        return -1;
//    }
//
//    /* Creates an answer for the host's IP, depending on the qtype */
//    // MARK: MUST contain all records for qtype ANY
//    header = pico_mdns_query_create_answer(local_addr, qtype, &len, name);
//    if (!header) {
//        mdns_dbg("Error occured while creating an answer (pico_err:%d)!\n", pico_err);
//        return -1;
//    }
//    
//    /* Send a response on the wire */
//    if(pico_mdns_send_packet(header, len) != (int)len) {
//        mdns_dbg("Send error occurred!\n");
//        return -1;
//    }
    
    return 0;
}

/* Compare if the received query name is the same as the name currently assigned to this host */
static int pico_check_query_name(char *url)
{
    IGNORE_PARAMETER(url);
//    char addr[29] = { 0 };
    
//    /* Check if query is a normal query for this hostname*/
//    if(strcmp(url, mdns_global_host) == 0)
//        return 1;
//    
//    /* Convert 192.168.1.1 decimal to '192.168.1.1'-string */
//    pico_ipv4_to_string(addr, mdns_sock_ipv4->local_addr.ip4.addr);
//    
//    /* Mirror 192.168.1.1 to 1.1.168.192 */
//    pico_dns_mirror_addr(addr);
//    
//    /* Add a arpa-suffix */
//    memcpy(addr + strlen(addr), ".in-addr.arpa", 13);
//    
//    /* Check if request name is reverse query for this hostname */
//    if(strcmp(url, addr) == 0)
//        return 1;
    
    return 0;
}

/* Handle a single incoming query */
static int pico_mdns_handle_query(char *name, struct pico_dns_question_suffix *suf, struct pico_ip4 peer)
{
    IGNORE_PARAMETER(name);
    IGNORE_PARAMETER(suf);
    IGNORE_PARAMETER(peer);
//    struct pico_mdns_cookie *ck = NULL; // Temporary storage of query cookie
//    
//    /* Remove cache flush bit if set MARK: but why? */
//    suf->qclass &= short_be((uint16_t) ~0x8000);
//    
//    mdns_dbg("Query type: %u, class: %u\n", short_be(suf->qtype), short_be(suf->qclass));
//    
//    /* Check if host has assigned itself a name already */
//    if(mdns_global_host) {
//        
//        /* Check if queried name is the same as currently assigned name */
//        // TODO: Check for all the records with that name and for wich this host has authority (Not only A and PTR)
//        if(pico_check_query_name(name)) {
//            /* Query is either a normal query or a reverse resolution query for this host */
//            pico_mdns_reply_query(short_be(suf->qtype), peer, name);
//        } else {
//            /* Query is not meant for this host */
//            /* TODO: Passive Observation Of Failures (POOF) */
//            mdns_dbg("Received request for unknown hostname %s (my hostname: %s)\n", name, mdns_global_host);
//        }
//    } else {
//        /* Find a corresponding query currently being queried */
//        ck = pico_mdns_find_cookie(name, short_be(suf->qtype));
//        if(ck && ck->count < 3) {
//            /* TODO: Simultaneous Probe Tiebreaking */
//        } else {
//            mdns_dbg("Received query before init\n");
//        }
//    }
    
    return 0;
}

/* Parses an incoming packet */
static int pico_mdns_recv(void *buf, int buflen, struct pico_ip4 peer)
{
    /* Point to the DNS packet in the buffer */
    struct pico_dns_header *header = (struct pico_dns_header *) buf;
    
    /* Point to right after the header */
    char *ptr = (char *)header + sizeof(struct pico_dns_header);
    
    /* Depending on the header, we need to provide a query or answer struct */
    struct pico_dns_question_suffix *qsuf;
    struct pico_dns_res_record_suffix *asuf;
    
    /* Count of questions and answers in the packet */
    uint16_t i, qcount, acount;
    
    /* Pointer to the data field of the questions or answers */
    char *data;
    
    /* Determine the count of questions and answers */
    // TODO: Authority Count and Additional count should be implemented
    qcount = short_be(header->qdcount);
    acount = short_be(header->ancount);
    mdns_dbg("\n>>>>>>> QDcount: %u, ANcount: %u\n", qcount, acount);
    if(qcount == 0 && acount == 0) {
        mdns_dbg("Query and answer count is 0!\n");
        return -1;
    }
    
    /* Handle queries */
    for(i = 0; i < qcount; i++) {
        /* Point to the qtype & qclass fields */
        qsuf = (struct pico_dns_question_suffix*) (ptr + pico_dns_namelen_comp(ptr) + 1);
        
        /* Convert 3www6google3com0 to .www.google.com */
        pico_dns_notation_to_name(ptr);
        if (!ptr)
            return -1;
        
        /* Handle the query accordingly (+1 to skip the first '.') */
        pico_mdns_handle_query(ptr + 1, qsuf, peer);
        
        /* Point to the next question */
        ptr = (char *)qsuf + sizeof(struct pico_dns_question_suffix);
        if(ptr - (char *)header > buflen) {
            mdns_dbg("buffer is too short! ptr offset=%ld buflen=%d\n", ptr - (char*)header, buflen);
            return -1;
        }
    }
    
    /* Handle answers */
    for(i = 0; i < acount; i++) {
        char *name;
        
        /* Point to the suffix of the answer contain in the answer section */
        asuf = (struct pico_dns_res_record_suffix*) (ptr + pico_dns_namelen_comp(ptr) + 1);
        
        /* Get the uncompressed name of the possibly compressed name contained in de rrname-field */
        if((name = pico_dns_expand_name_comp(ptr, header)) == NULL) {
            mdns_dbg("Received a zero name pointer\n");
            return -1;
        }
        
        /* Point to the data-field of the answer contained in the answer section */
        data = (char *)asuf + sizeof(struct pico_dns_res_record_suffix);
        
        /* Handle the answer accordingly (+1 to skip the first '.') */
        pico_mdns_handle_answer(name + 1, asuf, data);
        
        /* Free memory */
        PICO_FREE(name);
        
        /* Move to the next answer */
        ptr = data + short_be(asuf->rdlength);
        if(ptr - (char *)header > buflen) {
            mdns_dbg("buffer is too short! ptr offset=%ld buflen=%d\n", ptr - (char*)header, buflen);
            return -1;
        }
    }
    
    return 0;
}

/* Callback for UDP IPv4 socket events */
static void pico_mdns_event4( uint16_t ev, struct pico_socket *s )
{
    // MARK: Why MTU 1400 and not 1500?
    char recvbuf[PICO_MDNS_MTU] = { 0 }; // MTU of 1400
    struct pico_ip4 peer = { 0 };        // Peer who sent the data
    int pico_read = 0;                   // Count of readed bytes
    uint16_t port = 0;                   // Source port
    char host[30];                       // IP-address string
    
    /* process read event, data available */
    if (ev == PICO_SOCK_EV_RD) {
        mdns_dbg("READ EVENT!\n");
        /* Receive while data is available in socket buffer */
        while((pico_read = pico_socket_recvfrom(s, recvbuf, PICO_MDNS_MTU, &peer, &port)) > 0) {
            pico_ipv4_to_string(host, peer.addr);
            mdns_dbg("Received data from %s:%u\n", host, short_be(port));
            /* Handle the MDNS data received */
            pico_mdns_recv(recvbuf, pico_read, peer);
        }
    } else if (ev == PICO_SOCK_EV_CLOSE) {
        mdns_dbg("Socket is closed. Bailing out.\n");
        return;
    } else {
        mdns_dbg("Socket Error received. Bailing out.\n");
        return;
    }
}

// MARK: CACHE FUNCTIONS

int pico_mdns_flush_cache(void)
{
    struct pico_mdns_cache_rr *rr = NULL;
    struct pico_tree_node *index = NULL;
    
    mdns_dbg("Flushing mDNS RR cache\n");
    pico_tree_foreach(index, &CacheTable) {
        rr = index->keyValue;
        mdns_dbg("Deleting '%s' (%d)\n", rr->url, rr->suf->rtype);
        pico_tree_delete(&CacheTable, rr);
        pico_timer_cancel(rr->timer);
        PICO_FREE(rr->url);
        PICO_FREE(rr->suf);
        PICO_FREE(rr->rdata);
        PICO_FREE(rr);
    }
    return 0;
}

// MARK: ADDRESS RESOLUTION

static int pico_mdns_getaddr_generic(const char *url, void (*callback)(char *ip, void *arg), void *arg, uint16_t proto)
{
    struct pico_dns_header *header = NULL;
    uint16_t len = 0;
    
    IGNORE_PARAMETER(callback);
    IGNORE_PARAMETER(arg);
    IGNORE_PARAMETER(proto);
    
    if (!url) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    if(!mdns_sock_ipv4) {
        mdns_dbg("mDNS socket not yet populated. Did you call pico_mdns_init()?\n");
        return -1;
    }
    
    //header = pico_mdns_create_query(url, &len, proto, PICO_DNS_TYPE_PTR, PICO_MDNS_QUESTION_FLAG_NO_PROBE);
    if(!header || !len) {
        mdns_dbg("ERROR: mdns_create_query returned NULL\n");
        return -1;
    }
    
    if(pico_mdns_send_packet(header, len) != (int)len) {
        mdns_dbg("Send error!\n");
        return -1;
    }
    
    return 0;
}

static int pico_mdns_getname_generic(const char *ip, void (*callback)(char *url, void *arg), void *arg, uint16_t proto)
{
    struct pico_dns_header *header = NULL;
    uint16_t len = 0;
    
    IGNORE_PARAMETER(callback);
    IGNORE_PARAMETER(arg);
    IGNORE_PARAMETER(proto);
    
    if (!ip) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    if(!mdns_sock_ipv4) {
        mdns_dbg("Mdns socket not yet populated. Did you call pico_mdns_init()?\n");
        return -1;
    }
    
    //header = pico_mdns_create_query(ip, &len, proto, PICO_DNS_TYPE_PTR, PICO_MDNS_QUESTION_FLAG_NO_PROBE);
    if(!header || !len) {
        mdns_dbg("ERROR: mdns_create_query returned NULL\n");
        return -1;
    }
    
    if(pico_mdns_send_packet(header, len) != (int)len) {
        mdns_dbg("Send error!\n");
        return -1;
    }
    
    return 0;
}

int pico_mdns_getaddr(const char *url, void (*callback)(char *ip, void *arg), void *arg)
{
    struct pico_mdns_cache_rr *rr = NULL;
    char addr[46];
    rr = pico_mdns_cache_find_rr(url, PICO_DNS_TYPE_A);
    
    if(rr && rr->rdata) {
        pico_ipv4_to_string(addr, long_from(rr->rdata));
        mdns_dbg("Cache hit! Found A record for '%s' with addr '%s'\n", url, addr);
        callback(addr, arg);
        return 0;
    }
    else {
        mdns_dbg("Cache miss for A record - url '%s'\n", url);
        return pico_mdns_getaddr_generic(url, callback, arg, PICO_PROTO_IPV4);
    }
}

int pico_mdns_getname(const char *ip, void (*callback)(char *url, void *arg), void *arg)
{
    return pico_mdns_getname_generic(ip, callback, arg, PICO_PROTO_IPV4);
}

#ifdef PICO_SUPPORT_IPV6
int pico_mdns_getaddr6(const char *url, void (*callback)(char *ip, void *arg), void *arg)
{
    struct pico_mdns_cache_rr *rr = NULL;
    char addr[46];
    rr = pico_mdns_cache_find_rr(url, PICO_DNS_TYPE_AAAA);
    
    if(rr && rr->rdata) {
        pico_ipv6_to_string(addr, (uint8_t *)rr->rdata);
        mdns_dbg("Cache hit! Found AAAA record for '%s' with addr '%s'\n", url, addr);
        callback(addr, arg);
        return 0;
    }
    else {
        mdns_dbg("Cache miss for AAAA record - url '%s'\n", url);
        return pico_mdns_getaddr_generic(url, callback, arg, PICO_PROTO_IPV6);
    }
}

int pico_mdns_getname6(const char *ip, void (*callback)(char *url, void *arg), void *arg)
{
    return pico_mdns_getname_generic(ip, callback, arg, PICO_PROTO_IPV6);
}
#endif

// MARK: ANNOUNCEMENT

/* Utility function to send an announcement on the wire */
static int pico_mdns_send_packet_announcement(void)
{
//    /* mDNS headers are just the same as plain legacy DNS headers */
//    struct pico_dns_header *packet = NULL;
//    struct pico_dns_res_record *announcement = NULL;
//    
//    uint16_t len = 0; // Temporary storage of packet length
//    
//    /* Create an resource record to put in the announcement */
//    announcement = pico_mdns_rr_create(mdns_global_host, &mdns_sock_ipv4->local_addr, &len, PICO_DNS_TYPE_A, 120, (PICO_MDNS_RES_RECORD_UNIQUE));
//    if (!announcement) {
//        mdns_dbg("ERROR: mdns_create_query returned NULL\n");
//        return -1;
//    }
//
//    /* Create an mDNS answer */
//    packet = pico_mdns_answer_create(announcement, NULL, NULL, &len);
//    if(!packet) {
//        mdns_dbg("Could not create answer!\n");
//        return -1;
//    }
//    
//    /* Send the mDNS answer unsollicited via multicast */
//    if(pico_mdns_send_packet(packet, len) != (int)len) {
//        mdns_dbg("send error occured!\n");
//        return -1;
//    }
    
    return 0;
}

/* Callback function for the announcement timer */
static void pico_mdns_announce_timer(pico_time now, void *arg)
{
    IGNORE_PARAMETER(now);
    IGNORE_PARAMETER(arg);
    
    /* Send a second unsollicited announcement */
    pico_mdns_send_packet_announcement();
}

/* announce the local hostname to the network */
static int pico_mdns_announce( pico_mdns_res_record_list *records )
{
    IGNORE_PARAMETER(records);
    
//    /* Send a first unsollicited announcement */
//    if (pico_mdns_send_packet_announcement() < 0)
//        return -1;
//    
//    /* RFC:
//     *  The Multicast DNS responder MUST send at least two unsolicited
//     *  responses, one second apart.
//     */
//    pico_timer_add(1000, pico_mdns_announce_timer, NULL);
    
    return 0;
}

// MARK: PROBING

/* Callback function for the probe timer */
static void pico_mdns_probe_timer(pico_time now, void *arg)
{
    struct pico_mdns_cookie *ck = NULL;
    char ok[] = "OK";
    char temp[255] = { 0 };
    
    /* Cast the argument given in [arg] to a mDNS-cookie */
    if (!arg)
        return;
    ck = (struct pico_mdns_cookie *) arg;
    
    IGNORE_PARAMETER(now);
    
    if(!ck) {
        mdns_dbg("Cookie does not exist! This shouldn't happen\n");
        return;
    }

    /* If probe flag is reset */
    if(!IS_QUESTION_PROBE_FLAG_SET(ck->flags)) {
        mdns_dbg("Hostname already in use!\n");
        ck->callback(NULL, ck->arg);
        return;
    }
    
    /* After 3 successful probing attempts */
    if(ck->count == 0) {
//        mdns_global_host = PICO_ZALLOC(strlen(ck->qname) - 1);
//        if (!mdns_global_host) {
//            pico_err = PICO_ERR_ENOMEM;
//            return;
//        }
        strcpy(temp, ck->qname);
        pico_dns_notation_to_name(temp);
        mdns_dbg("Count is zero! Claimed %s\n", temp);
        //pico_mdns_announce();
    
        ck->callback(ok, ck->arg);
        pico_mdns_del_cookie(ck->qname, ck->qtype);
        
        return;
    }
    
    /* Send Probing query */
    if(pico_mdns_send_packet(ck->packet, ck->len) != (int)ck->len) {
        mdns_dbg("Send error occurred!\n");
        PICO_FREE(arg);
        ck->callback(NULL, ck->arg);
        return;
    }
    
    mdns_dbg("Probed!\n");
    
    /* Decrement probe count */
    ck->count--;
    
    /* Schedule the next probe after 250 ms */
    pico_timer_add(250, pico_mdns_probe_timer, ck);
}

static struct pico_dns_question *pico_mdns_probe_list_find_probe( struct pico_dns_question *probe_list,
                                                                  char *qname )
{
    struct pico_dns_question *iterator = NULL;
    struct pico_dns_question *found = NULL;
    
    while (iterator) {
        if (strcmp(iterator->qname, qname) == 0) found = iterator;
    }
    
    return found;
}

/*  */
static struct pico_dns_question *pico_mdns_probe_list_create( pico_mdns_res_record_list *records,
                                                             struct pico_dns_res_record **authority_list,
                                                             struct pico_dns_res_record **additional_list)
{
    struct pico_mdns_res_record *iterator = NULL;
    struct pico_dns_question *probe_list = NULL;
    struct pico_dns_question **qiterator = NULL;
    struct pico_dns_res_record **authiterator = NULL;
    uint16_t qlen = 0;
    uint16_t rlen = 0;
    
    IGNORE_PARAMETER(additional_list);
    
    iterator = records;
    
    while (iterator) {
        if (IS_RES_RECORD_FLAG_CLAIM_UNIQUE_SET(iterator->flags)) {
            /* Put question iterator at beginning of question list */
            qiterator = &probe_list;
            
            /* Put authority iterator at beginning of authority list */
            authiterator = authority_list;
            
            /* Iterate until the end of question list */
            while (*qiterator && *authiterator) {
                *qiterator = (*qiterator)->next;
                *authiterator = (*authiterator)->next;
            }
            
            /* If there is not already a probe query for this name in the list */
            if (!pico_mdns_probe_list_find_probe(probe_list, iterator->record->rname))
            {
                /* Create a new question at the end of the list */
                /* RFC:
                 *  Probe querys SHOULD be sent with as "QU" questions with the unicast-response bit set.
                 *  To a defending host to respond immediately via unicast, instead of potentially
                 *  having to wait before replying via multicast.
                 */
                *qiterator = pico_mdns_question_create(pico_dns_qname_to_url(iterator->record->rname),
                                                       &qlen,
                                                       PICO_PROTO_IPV4,
                                                       PICO_DNS_TYPE_ANY,
                                                       (PICO_MDNS_QUESTION_FLAG_PROBE | PICO_MDNS_QUESTION_FLAG_UNICAST_RES));
            }
        
            /* Add an authority record to the end of the list */
            *authiterator = iterator->record;
            
            /* Don't want a cache flush in the authority section */
            PICO_MDNS_CLR_MSB_BE((*authiterator)->rsuffix->rclass);
        } else {
            /* You don't need to probe Shared Records */
            PICO_MDNS_SET_FLAG(iterator->flags, PICO_MDNS_RES_RECORD_PROBED);
        }
        
        /* Move to next mDNS resource record */
        iterator = iterator->next;
    }
    
    return probe_list;
}

/* **************************************************************************
 *  Claim several mDNS resource records at once.
 * **************************************************************************/
int pico_mdns_claim( pico_mdns_res_record_list *records,
                    void (*cb_claimed)(char *str, void *arg),
                    void *arg )
{
    struct pico_dns_question *probe_list = NULL;        /* Questions to put in probe query */
    struct pico_dns_res_record *authority_list = NULL;  /* Authority records to put in probe query */
    struct pico_mdns_cookie *cookie = NULL;             /* */
    pico_dns_packet *packet = NULL;
    uint16_t len = 0;
    char *OK = PICO_OK_STRING, *NOK = PICO_NOK_STRING, *SOK = PICO_SOK_STRING;
    
    /* Check if arguments are passed correctly */
    if (!records || !cb_claimed) {
        mdns_dbg("NULL pointers passed to 'pico_mdns_claim()'!\n");
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Check if module is initialised */
    if (!mdns_sock_ipv4) {
        mdns_dbg("mDNS socket not initialised, did you call 'pico_mdns_init()'?\n");
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Update the rr's and create a probe list */
    probe_list = pico_mdns_probe_list_create(records, &authority_list, NULL);
    if (!probe_list || !authority_list) {
        mdns_dbg("ERROR: mdns_probe_list_create returned NULL!\n");
    }
    
    
    
    /* Create a multiquestion query with authority records */
    packet = pico_mdns_query_create(probe_list, NULL, authority_list, NULL, &len);
    if (!packet) {
        mdns_dbg("ERROR: mdns_query_create returned NULL!\n");
    }
    
    /* Add a cookie to the global tree so we don't have to create the query everytime */
    cookie = pico_mdns_add_cookie(packet,
                                  len,
                                  (PICO_MDNS_QUESTION_FLAG_PROBE | PICO_MDNS_QUESTION_FLAG_UNICAST_RES),
                                  3,
                                  cb_claimed,
                                  arg);
    if (!cookie) {
        mdns_dbg("ERROR: mdns_add_cookie returned NULL\n");
        return -1;
    }
    
    /* RFC:
     *  When the host is ready to send his probe query he SHOULD delay it's
     *  transmission with a randomly chosen time between 0 and 250 ms.
     */
    pico_timer_add(pico_rand() % 250, pico_mdns_probe_timer, cookie);
    
    return 0;
}

/* **************************************************************************
 *  Initialises the global mDNS socket. Calls cb_initialised when succeeded.
 *  [flags] is for future use. f.e. Opening a IPv4 multicast socket or an
 *  IPv6 one or both.
 * **************************************************************************/
int pico_mdns_init( uint8_t flags,
                    void (*cb_initialised)(char *str, void *arg),
                    void *arg )
{
    struct pico_ip_mreq mreq4;
    char *OK = PICO_OK_STRING, *NOK = PICO_NOK_STRING, *SOK = PICO_SOK_STRING;
    uint16_t proto4 = PICO_PROTO_IPV4;
    uint16_t port = 0;
    uint16_t loop = 0;   // Loopback = 0
    uint16_t ttl = 255;  // IP TTL SHOULD = 255
    
    /* For now */
    IGNORE_PARAMETER(flags);
    
    /* Initialise port */
    port = short_be(mdns_port);
    
    /* Check callbcak parameter */
    if(!cb_initialised) {
        mdns_dbg("No callback function suplied!\n");
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Open global IPv4 mDNS socket */
    mdns_sock_ipv4 = pico_socket_open(proto4, PICO_PROTO_UDP, &pico_mdns_event4);
    if(!mdns_sock_ipv4) {
        mdns_dbg("Open returned empty IPv4 socket\n");
        return -1;
    }
    
    /* Convert the mDNS IPv4 destination address to struct */
    if(pico_string_to_ipv4(PICO_MDNS_DEST_ADDR4, &mreq4.mcast_group_addr.addr) != 0) {
        mdns_dbg("String to IPv4 error\n");
        return -1;
    }
    
    /* Receive data on any network interface */
    mreq4.mcast_link_addr = inaddr_any;
    
    /* Don't want the multicast data to be looped back to the host */
    if(pico_socket_setoption(mdns_sock_ipv4, PICO_IP_MULTICAST_LOOP, &loop) < 0) {
        mdns_dbg("socket_setoption PICO_IP_MULTICAST_LOOP failed\n");
        return -1;
    }
    
    /* Tell the kernel we're interested in this particular multicast group */
    if(pico_socket_setoption(mdns_sock_ipv4, PICO_IP_ADD_MEMBERSHIP, &mreq4) < 0) {
        mdns_dbg("socket_setoption PICO_IP_ADD_MEMBERSHIP failed\n");
        return -1;
    }
    
    /* RFC:
     *  All multicast responses (including answers sent via unicast) SHOULD
     *  be send with IP TTL set to 255 for backward-compatibility reasons
     */
    if(pico_socket_setoption(mdns_sock_ipv4, PICO_IP_MULTICAST_TTL, &ttl) < 0) {
        mdns_dbg("socket_setoption PICO_IP_MULTICAST_TTL failed\n");
        return -1;
    }
    
    /* Bind to mDNS port */
    if (pico_socket_bind(mdns_sock_ipv4, &inaddr_any, &port) != 0) {
        mdns_dbg("Bind error!\n");
        return -1;
    }
    
    /* Call callback */
    cb_initialised(OK, arg);
    
    return 0;
}

#endif /* PICO_SUPPORT_MDNS */
