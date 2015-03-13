/*********************************************************************
   PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
   See LICENSE and COPYING for usage.

   .

   Authors: Roel Postelmans
 *********************************************************************/

#ifndef INCLUDE_PICO_MLD
#define INCLUDE_PICO_MLD

#define PICO_MLDV1               1
#define PICO_MLDV2               2

#define PICO_MLD_QUERY  130
#define PICO_MLD_REPORT 131
#define PICO_MLD_DONE   132
#define PICO_MLD_REPORTV2 143

/*RFC 3810 $6.2 */
#define MLD_HOP_LIMIT 1

/* states */
#define MLD_STATE_NON_LISTENER              (0x0)
#define MLD_STATE_DELAYING_LISTENER         (0x1)
#define MLD_STATE_IDLE_LISTENER             (0x2)

/* group record types */
#define MLD_MODE_IS_INCLUDE                  (1)
#define MLD_MODE_IS_EXCLUDE                  (2)
#define MLD_CHANGE_TO_INCLUDE_MODE           (3)
#define MLD_CHANGE_TO_EXCLUDE_MODE           (4)
/* events */
#define MLD_EVENT_QUERY_RECV                (0x0)
#define MLD_EVENT_DONE_RECV                 (0x1)
#define MLD_EVENT_REPORT_RECV               (0x2)
#define MLD_EVENT_TIMER_EXPIRED             (0x3)
#define MLD_EVENT_STOP_LISTENING            (0x4)
#define MLD_EVENT_START_LISTENING           (0x5)

/* (default) Variabels for times/counters */
/* ALL IN SECONDS */
#define MLD_ROBUSTNESS                      (2)
#define MLD_QUERY_INTERVAL                  (125)
#define MLD_QUERY_RESPONSE_INTERVAL         (10)
#define MLD_DEFAULT_MAX_RESPONSE_TIME       (100)
#define MLD_MULTICAST_LISTENER_INTERVAL     (MLD_ROBUSTNESS * MLD_QUERY_INTERVAL) + MLD_QUERY_RESPONSE_INTERVAL
#define MLD_OTHER_QUERIER_PRESENT_INTERVAL  (MLD_ROBUSTNESS * MLD_QUERY_INTERVAL) + (0.5 * MLD_QUERY_RESPONSE_INTERVAL)
#define MLD_STARTUP_QUERY_INTERVAL          (0.25 * MLD_QUERY_INTERVAL)
#define MLD_STARTUP_QUERY_COUNT             MLD_ROBUSTNESS
#define MLD_LAST_LISTENER_QUERY_INTERVAL    1
#define MLD_LISTENER_QUERY_COUNT            MLD_ROBUSTNESS
#define MLD_UNSOLICITED_REPORT_INTERVAL     10

/* custom timers types */
#define MLD_TIMER_GROUP_REPORT           (1)
#define MLD_TIMER_V1_REPORT             (2)
#define MLD_TIMER_V2_REPORT             (3)


/* Who has send the last report message */
#define MLD_HOST_LAST                    (0x1)
#define MLD_HOST_NOT_LAST                (0x0)

#define IP_OPTION_ROUTER_ALERT_LEN        (8)

extern struct pico_protocol pico_proto_mld;

struct mld_multicast_address_record {
    uint8_t type;
    uint8_t aux_len;
    uint16_t nbr_src;
    struct pico_ip6 multicast;
    struct pico_ip6 src[0];
};

struct mld_parameters {
    uint8_t event; 
    uint8_t state;
    uint8_t general_query;
    uint8_t last_host;
    uint8_t max_resp_time;
    struct pico_ip6 mcast_link;
    struct pico_ip6 mcast_group;
    struct pico_tree *MCASTFilter;
    struct pico_frame *f;
};

struct mld_timer {
    uint8_t type;
    uint8_t stopped;
    pico_time start;
    pico_time delay;
    struct pico_ip6 mcast_link;
    struct pico_ip6 mcast_group;
    struct pico_frame *f;
    void (*callback)(struct igmp_timer *t);
};


#endif /* _INCLUDE_PICO_MLD */
