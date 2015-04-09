#include "utils.h"
#include <pico_dns_common.h>
#include <pico_mdns.h>
#include <pico_ipv4.h>
#include <pico_addressing.h>

/*** START MDNS ***/

#ifdef PICO_SUPPORT_MDNS

#define DEBUG(s, args...) printf("pico_err: %d: %s", pico_err, s, ##args)

void mdns_getname_callback(char *str, void *arg)
{
    
}

void mdns_getaddr_callback(char *str, void *arg)
{
    if (!str)
        printf("Getaddr: timeout occurred!\n");
    else
        printf("Getaddr callback called, str: %s\n", str);
}

void mdns_claimed_callback(char *str, void *arg)
{
    printf("Claimed: %s\n", str);
}

void mdns_init_callback(pico_mdns_record_vector *vector, char *str, void *arg)
{
    char *hostname = NULL;
    struct pico_mdns_record *hostname_record = NULL;
    hostname_record = pico_mdns_record_vector_get(vector, 0);
    hostname = pico_dns_qname_to_url(hostname_record->record->rname);
    printf("Initialised with hostname: %s\n", hostname);
    PICO_FREE(hostname);
}

void app_mdns(char *arg, struct pico_ipv4_link *link)
{
    char *hostname, *peername;
    char *nxt = arg;
    struct pico_ip6 ipaddr6 = {{0}};
    
    if (!nxt)
        exit(255);

    nxt = cpy_arg(&hostname, nxt);
    if(!hostname) {
        exit(255);
    }

    if(!nxt) {
        printf("Not enough args supplied!\n");
        exit(255);
    }

    nxt = cpy_arg(&peername, nxt);
    if(!peername) {
        exit(255);
    }
    
    if (!link) {
        printf("Link not found!\n");
        exit(255);
    }
    
    printf("\nStarting mDNS module...\n");
    if (pico_mdns_init(hostname, link, 0, &mdns_init_callback, peername) != 0) {
        printf("Initialisation returned with Error!\n");
        exit(255);
    }
    printf("DONE - Initialising mDNS module.\n");
//    
//    /* Create a resource record you want to register */
//    if (pico_mdns_res_record_list_append_create(hostname,
//                                                (void*)(&(link->address)),
//                                                PICO_DNS_TYPE_A,
//                                                120,
//                                                PICO_MDNS_RES_RECORD_SHARED,
//                                                &records) != 0) {
//        DEBUG("Could not create mDNS resource record!\n");
//        exit(255);
//    }
//    
//    /* Create a resource record you want to register */
//    if (pico_mdns_res_record_list_append_create("foo.local",
//                                                (void*)(&(link->address)),
//                                                PICO_DNS_TYPE_A,
//                                                120,
//                                                PICO_MDNS_RES_RECORD_SHARED,
//                                                &records) != 0) {
//        DEBUG("Could not create mDNS resource record!\n");
//        exit(255);
//    }
//    
//    /* Create a resource record you want to register */
//    if (pico_mdns_res_record_list_append_create("bar.local",
//                                                (void*)(&(link->address)),
//                                                PICO_DNS_TYPE_A,
//                                                120,
//                                                PICO_MDNS_RES_RECORD_UNIQUE,
//                                                &records) != 0) {
//        DEBUG("Could not create mDNS resource record!\n");
//        exit(255);
//    }
//    
//    /* Create a resource record you want to register */
//    if (pico_mdns_res_record_list_append_create("test.local",
//                                                (void*)(&(link->address)),
//                                                PICO_DNS_TYPE_A,
//                                                120,
//                                                PICO_MDNS_RES_RECORD_UNIQUE,
//                                                &records) != 0) {
//        DEBUG("Could not create mDNS resource record!\n");
//        exit(255);
//    }
//    
//    pico_string_to_ipv6("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
//                        ipaddr6.addr);
//    
//    /* Create a resource record you want to register */
//    if (pico_mdns_res_record_list_append_create("test.local",
//                                                (void*)(&ipaddr6),
//                                                PICO_DNS_TYPE_AAAA,
//                                                120,
//                                                PICO_MDNS_RES_RECORD_UNIQUE,
//                                                &records) != 0) {
//        DEBUG("Could not create mDNS resource record!\n");
//        exit(255);
//    }
//    
//    /* Check if records is not still empty */
//    if (!records) {
//        printf("Creating a mDNS resource record returned NULL!\n");
//    }
//    
//    /* Try to claim it */
//    if (pico_mdns_claim(records, &mdns_claimed_callback, peername) != 0) {
//        printf("Could not claim mDNS resource record!\n");
//        exit(255);
//    }
    
    while(1) {
        pico_stack_tick();
        usleep(2000);
    }
}
#endif
/*** END MDNS ***/
