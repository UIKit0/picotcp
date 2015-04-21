/* ****************************************************************************
 *  PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
 *  See LICENSE and COPYING for usage.
 *
 *  .
 *
 *  Authors: Toon Stegen
 * ****************************************************************************/
#include "pico_config.h"
#include "pico_stack.h"
#include "pico_addressing.h"
#include "pico_socket.h"
#include "pico_ipv4.h"
#include "pico_ipv6.h"
#include "pico_dns_common.h"
#include "pico_dns_client.h"
#include "pico_tree.h"

//#define dns_dbg(...) do {} while(0)
#define dns_dbg dbg

// MARK: PROTOTYPES
static int
pico_dns_record_copy_flat( struct pico_dns_record *record,
                           uint8_t **destination );
static char *
pico_dns_url_to_reverse_qname( const char *url, uint8_t proto );

// MARK: DNS PACKET FUNCTIONS

/* ****************************************************************************
 *  Fills the header section of a DNS packet with correct flags and section-
 *  counts.
 * ****************************************************************************/
void
pico_dns_fill_packet_header( struct pico_dns_header *hdr,
                             uint16_t qdcount,
                             uint16_t ancount,
                             uint16_t nscount,
                             uint16_t arcount )
{
    /* hdr->id should be filled by caller */
    
    /* If there are questions in the packet, make it a Query */
    if(qdcount > 0) {
        hdr->qr = PICO_DNS_QR_QUERY;
        hdr->aa = PICO_DNS_AA_NO_AUTHORITY;
    } else {
    /* If there are questions in the packet, make it a Response */
        hdr->qr = PICO_DNS_QR_RESPONSE;
        hdr->aa = PICO_DNS_AA_IS_AUTHORITY;
    }
    
    /* Fill in the flags and the fields */
    hdr->opcode = PICO_DNS_OPCODE_QUERY;
    hdr->tc = PICO_DNS_TC_NO_TRUNCATION;
    hdr->rd = PICO_DNS_RD_NO_DESIRE;
    hdr->ra = PICO_DNS_RA_NO_SUPPORT;
    hdr->z = 0; /* Z, AD, CD are 0 */
    hdr->rcode = PICO_DNS_RCODE_NO_ERROR;
    hdr->qdcount = short_be(qdcount);
    hdr->ancount = short_be(ancount);
    hdr->nscount = short_be(nscount);
    hdr->arcount = short_be(arcount);
}

/* ****************************************************************************
 *  Fills the resource record section of a DNS packet with provided record-
 *  vectors.
 * ****************************************************************************/
static int
pico_dns_fill_packet_rr_sections( pico_dns_packet *packet,
                                  pico_dns_question_vector *qvector,
                                  pico_dns_record_vector *anvector,
                                  pico_dns_record_vector *nsvector,
                                  pico_dns_record_vector *arvector )
{
    struct pico_dns_record *record = NULL;
    uint16_t i = 0;
    uint8_t *destination = NULL;

    /* Check params */
    if (!packet || !qvector || !anvector || !nsvector || !arvector) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Initialise the destination pointers before iterating */
    destination = (uint8_t *)packet + sizeof(struct pico_dns_header);
    destination += pico_dns_question_vector_size(qvector);
    
    /* iterate over ANSWER vector */
    for (i = 0; i < pico_dns_record_vector_count(anvector); i++) {
        record = pico_dns_record_vector_get(anvector, i);
        if (pico_dns_record_copy_flat(record, &destination)) {
            dns_dbg("Could not copy record into Answer Section!\n");
            return -1;
        }
    }
    
    /* iterate over AUTHORITY vector */
    for (i = 0; i < pico_dns_record_vector_count(nsvector); i++) {
        record = pico_dns_record_vector_get(nsvector, i);
        if (pico_dns_record_copy_flat(record, &destination)) {
            dns_dbg("Could not copy record into Authority Section!\n");
            return -1;
        }
    }

    /* iterate over ADDITIONAL vector */
    for (i = 0; i < pico_dns_record_vector_count(arvector); i++) {
        record = pico_dns_record_vector_get(nsvector, i);
        if (pico_dns_record_copy_flat(record, &destination)) {
            dns_dbg("Could not copy record into Authority Section!\n");
            return -1;
        }
    }

    return 0;
}

/* ****************************************************************************
 *  Fills the question section of a DNS packet with provided questions in
 *  question_list
 * ****************************************************************************/
static int
pico_dns_fill_packet_question_section( pico_dns_packet *packet,
                                       pico_dns_question_vector *vector)
{
    struct pico_dns_question *question = NULL;
    struct pico_dns_question_suffix *destination_qsuffix = NULL;
    char *destination_qname = NULL;
    uint16_t i = 0;
    
    /* Check params */
    if (!packet || !vector) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    /* Get the first question in the vector */
    question = pico_dns_question_vector_get(vector, 0);
    if (!question) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    destination_qname = (char *)((uint8_t *)packet +
                                 sizeof(struct pico_dns_header));
    destination_qsuffix = (struct pico_dns_question_suffix *)
                          (destination_qname + question->qname_length);

    for (i = 0; i < pico_dns_question_vector_count(vector); i++) {
        question = pico_dns_question_vector_get(vector, i);

        /* Copy the qname of the question into the packet */
        memcpy(destination_qname, question->qname, question->qname_length + 1u);
        
        /* Copy the qtype and qclass fields */
        destination_qsuffix->qtype = question->qsuffix->qtype;
        destination_qsuffix->qclass = question->qsuffix->qclass;

        /* Set the destination pointers correctly */
        destination_qname = (char *)((uint8_t *) destination_qsuffix +
                                     sizeof(struct pico_dns_question_suffix));
        destination_qsuffix = (struct pico_dns_question_suffix *)
                              (destination_qname + question->qname_length);
    }

    return 0;
}

/* ****************************************************************************
 *  Looks for a name somewhere else in packet, more specifically between the
 *  beginning of the data buffer and the name itself.
 * ****************************************************************************/
static uint8_t *
pico_dns_packet_compress_find_ptr( uint8_t *name,
                                   uint8_t *data,
                                   uint16_t len )
{
    uint8_t *iterator = NULL;
    
    /* Check params */
    if (!name || !data || !len) {
        return NULL;
    }
    if ((name < data) || (name > (data + len))) {
        return NULL;
    }
    
    iterator = data;
    
    /* Iterate from the beginning of data up until the name-ptr */
    while (iterator < name) {
        /* Compare in each iteration of current name is equal to a section of
         the DNS packet and if so return the pointer to that section */
        if (memcmp((void *)iterator++,
                   (void *)name,
                   strlen((char *)name) + 1u) == 0)
            return (iterator - 1);
    }
    
    return NULL;
}

/* ****************************************************************************
 *  Compresses a single name by looking for the same name somewhere else in the
 *  packet-buffer.
 * ****************************************************************************/
static int
pico_dns_packet_compress_name( uint8_t *name,
                               uint8_t *packet,
                               uint16_t *len)
{
    uint8_t *lbl_iterator = NULL;    // To iterate over labels
    uint8_t *compression_ptr = NULL; // PTR to somewhere else in the packet
    uint8_t *offset = NULL;          // PTR after compression pointer
    uint16_t ptr = 0;                // DNS name compression pointer
    uint8_t lbl_length = 0;          // Temporary lable length
    
    /* Check params */
    if (!name || !packet || !len) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    if ((name < packet) || (name > (packet + *len))) {
        dns_dbg("Name ptr OOB. name: %p max: %p\n", name, packet + *len);
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    /* Try to compress name */
    lbl_iterator = name;
    while (lbl_iterator != '\0') {
        /* Try to find a compression pointer with current name */
        compression_ptr = pico_dns_packet_compress_find_ptr(lbl_iterator,
                                                            packet + 12, *len);

        /* If name can be compressed */
        if (compression_ptr) {
            /* Calculate the compression pointer value */
            ptr = (uint16_t)(compression_ptr - packet);
            
            /* Temporarily store the current label length */
            lbl_length = *lbl_iterator;
            
            /* Set the compression pointer in the packet */
            *lbl_iterator = (uint8_t)(0xC0 | (uint8_t)(ptr >> 8));
            *(lbl_iterator + 1) = (uint8_t)(ptr & 0xFF);

            /* Move up the rest of the packet data to right after the pointer */
            offset = lbl_iterator + 2;
            
            /* Move to the next length-label, until a zero-byte */
            lbl_iterator += lbl_length + 1;
            while (*lbl_iterator != '\0')
                lbl_iterator = lbl_iterator + *(lbl_iterator) + 1;
            
            /* Copy data after zero-byte, to location right after pointer,
             previously set in offset */
            memcpy(offset, lbl_iterator + 1,
                   (size_t)(((uint8_t *)packet + *len) - lbl_iterator));
            *len = (uint16_t)(*len - (lbl_iterator - offset + 1));
            break;
        }
        
        /* Move to next length label */
        lbl_iterator = lbl_iterator + *(lbl_iterator) + 1;
    }
    
    return 0;
}

/* ****************************************************************************
 *  Applies DNS name compression to an entire DNS packet
 * ****************************************************************************/
static int
pico_dns_packet_compress( pico_dns_packet *packet, uint16_t *len )
{
    uint8_t *packet_buf = NULL;
    uint8_t *iterator = NULL;
    struct pico_dns_record_suffix *rsuffix = NULL;
    uint16_t qdcount = 0, ancount = 0, nscount = 0, arcount = 0, i = 0;
    
    /* Check params */
    if (!packet || !len) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    packet_buf = (uint8_t *)packet;
    
    /* Temporarily store the question & record counts */
    qdcount = short_be(packet->qdcount);
    ancount = short_be(packet->ancount);
    nscount = short_be(packet->nscount);
    arcount = short_be(packet->arcount);
    
    /* Move past the DNS packet header */
    iterator = (uint8_t *)((uint8_t *)packet + 12u);

    /* Start with the questions */
    for (i = 0; i < qdcount; i++) {
        /* First question can't be compressed */
        if (i > 0)
            pico_dns_packet_compress_name(iterator, packet_buf, len);

        /* Move to next question */
        iterator = (uint8_t *)(iterator +
                               pico_dns_namelen_comp((char *)iterator) +
                               sizeof(struct pico_dns_question_suffix) +
                               1u);
    }

    /* Then onto the answers */
    for (i = 0; i < ancount; i++) {
        if (qdcount > 0)
            /* When question are present, compress like normal */
            pico_dns_packet_compress_name(iterator, packet_buf, len);
        else {
            if (i > 0)
                /* Otherwise don't don't compress first iterator */
                pico_dns_packet_compress_name(iterator, packet_buf, len);
        }

        /* To get rdlength */
        rsuffix = (struct pico_dns_record_suffix *)
                  (iterator + pico_dns_namelen_comp((char *)iterator) + 1u);
        
        /* Move to next res record */
        iterator = (uint8_t *)((uint8_t *)rsuffix +
                               sizeof(struct pico_dns_record_suffix) +
                               short_be(rsuffix->rdlength));
    }
    
    /* Then onto the authorities */
    for (i = 0; i < nscount; i++) {
        if (qdcount > 0 || ancount > 0)
            pico_dns_packet_compress_name(iterator, packet_buf, len);
        else {
            if (i > 0)
                pico_dns_packet_compress_name(iterator, packet_buf, len);
        }

        /* To get rdlength */
        rsuffix = (struct pico_dns_record_suffix *)
                  (iterator + pico_dns_namelen_comp((char *)iterator) + 1u);

        /* Move to next res record */
        iterator = (uint8_t *)((uint8_t *)rsuffix +
                               sizeof(struct pico_dns_record_suffix) +
                               short_be(rsuffix->rdlength));
    }

    /* Then onto the additionals */
    for (i = 0; i < arcount; i++) {
        if (qdcount > 0 || ancount > 0 || nscount > 0)
            pico_dns_packet_compress_name(iterator, packet_buf, len);
        else {
            if (i > 0)
                pico_dns_packet_compress_name(iterator, packet_buf, len);
        }
        
        /* Try to compress name */
        pico_dns_packet_compress_name(iterator, packet_buf, len);
        
        /* To get rdlength */
        rsuffix = (struct pico_dns_record_suffix *)
                  (iterator + pico_dns_namelen_comp((char *)iterator) + 1u);

        /* Move to next res record */
        iterator = (uint8_t *)((uint8_t *)rsuffix +
                               sizeof(struct pico_dns_record_suffix) +
                               short_be(rsuffix->rdlength));
    }
    
    return 0;
}

// MARK: QUESTION FUNCTIONS

/* ****************************************************************************
 *  Fills the question fixed-sized flags & fields accordingly.
 * ****************************************************************************/
void
pico_dns_question_fill_qsuffix( struct pico_dns_question_suffix *suf,
                                uint16_t type,
                                uint16_t qclass )
{
    suf->qtype = short_be(type);
    suf->qclass = short_be(qclass);
}

/* ****************************************************************************
 *  Just copies a question provided in [questio]
 * ****************************************************************************/
struct pico_dns_question *
pico_dns_question_copy( struct pico_dns_question *question )
{
    struct pico_dns_question *copy = NULL;

    /* Check params */
    if (!question) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }

    /* Provide space for the copy and copy */
    copy = (struct pico_dns_question *)
            PICO_ZALLOC(sizeof(struct pico_dns_question));
    if (!copy) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    copy->qname = PICO_ZALLOC((size_t)question->qname_length);
    if (!(copy->qname)) {
        pico_err = PICO_ERR_ENOMEM;
        PICO_FREE(copy);
        return NULL;
    }
    strcpy(copy->qname, question->qname);
    copy->qname_length = question->qname_length;
    copy->qsuffix = PICO_ZALLOC(sizeof(struct pico_dns_question_suffix));
    if (!(copy->qsuffix)) {
        pico_err = PICO_ERR_ENOMEM;
        PICO_FREE(copy->qname);
        PICO_FREE(copy);
        return NULL;
    }
    copy->qsuffix->qtype = question->qsuffix->qtype;
    copy->qsuffix->qclass = question->qsuffix->qclass;
    copy->proto = question->proto;
    return copy;
}

/* ****************************************************************************
 *  Deletes & free's the memory for a certain dns resource record. Doesn't take
 *  lists into account so if applied to a list, most probably, gaps will arisee.
 * ****************************************************************************/
int
pico_dns_question_delete( struct pico_dns_question **question)
{
    if (!question || !(*question)) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    if ((*question)->qname)
        PICO_FREE((*question)->qname);
    (*question)->qname = NULL;
    
    if ((*question)->qsuffix)
        PICO_FREE((*question)->qsuffix);
    (*question)->qsuffix = NULL;
    
    PICO_FREE(*question);
    *question = NULL;
    
    return 0;
}

/* ****************************************************************************
 *  Creates a standalone DNS question for given 'url'. Fills the 'len'-argument
 *  with the total length of the question.
 * ****************************************************************************/
struct pico_dns_question *
pico_dns_question_create( const char *url,
                          uint16_t *len,
                          uint8_t proto,
                          uint16_t qtype,
                          uint16_t qclass,
                          uint8_t reverse )
{
    struct pico_dns_question *question = NULL;
    char *qname = NULL;
    uint16_t slen = 0;
    
    /* Check if valid arguments are provided */
    if (!url || !len) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }

    if (reverse && qtype == PICO_DNS_TYPE_PTR) {
        qname = pico_dns_url_to_reverse_qname(url, proto);
    } else {
        qname = pico_dns_url_to_qname(url);
    }
    slen = (uint16_t)(strlen(qname) + 1u);
    
    /* Allocate space for the question and the subfields */
    question = PICO_ZALLOC(sizeof(struct pico_dns_question));
    if (!question) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    question->qname = qname;
    question->qsuffix = PICO_ZALLOC(sizeof(struct pico_dns_question_suffix));
    if (!(question->qname) || !(question->qsuffix)) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    
    /* Determine the entire length of the question */
    *len = (uint16_t)(slen + (uint16_t)sizeof(struct pico_dns_question_suffix));
    
    /* Set the length of the question */
    question->qname_length = (uint8_t)(slen);
    
    /* Fill in the question suffix */
    pico_dns_question_fill_qsuffix(question->qsuffix, qtype, qclass);
    
    /* Fill in the proto */
    question->proto = proto;
    
    return question;
}

/* ****************************************************************************
 *  Initialise a DNS question vector
 * ****************************************************************************/
int
pico_dns_question_vector_init( pico_dns_question_vector *vector )
{
    /* Check params */
    if (!vector) return -1;
    vector->questions = NULL;
    vector->count = 0;
    return 0;
}

/* ****************************************************************************
 *  Returns the amount of questions contained in the DNS question vector
 * ****************************************************************************/
uint16_t
pico_dns_question_vector_count( pico_dns_question_vector *vector )
{
    /* Check params */
    if (!vector) return 0;
    return vector->count;
}

/* ****************************************************************************
 *  Adds a DNS question to a DNS question vector
 * ****************************************************************************/
int
pico_dns_question_vector_add( pico_dns_question_vector *vector,
                              struct pico_dns_question *question )
{
    struct pico_dns_question **new_questions = NULL;
    uint16_t i = 0;
    
    /* Check params */
    if (!vector || !question) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Create a new array with larger size */
    new_questions = PICO_ZALLOC(sizeof(struct pico_dns_question *) *
                                (vector->count + 1u));
    if (!new_questions)
        return -1;
    
    /* Copy all the record-pointers from the previous array to the new one */
    for (i = 0; i < vector->count; i++)
        new_questions[i] = vector->questions[i];
    new_questions[i] = question;
    
    /* Free the previous array */
    if (vector->questions)
        PICO_FREE(vector->questions);
    
    /* Set the records array to the new one and update count */
    vector->questions = new_questions;
    vector->count++;
    return 0;
}

/* ****************************************************************************
 *  Adds a copy of a DNS question to a DNS question vector
 * ****************************************************************************/
int
pico_dns_question_vector_add_copy( pico_dns_question_vector *vector,
                                   struct pico_dns_question *question )
{
    struct pico_dns_question *copy = NULL;
    
    /* Check params */
    if (!vector || !question) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Create copy */
    copy = pico_dns_question_copy(question);
    return pico_dns_question_vector_add(vector, copy);
}

/* ****************************************************************************
 *  Returns a DNS question from a DNS question vector at a certain index
 * ****************************************************************************/
struct pico_dns_question *
pico_dns_question_vector_get( pico_dns_question_vector *vector,
                              uint16_t index)
{
    /* Check params */
    if (!vector)
        return NULL;
    
    /* Return record with conditioned index */
    if (index < vector->count)
        return vector->questions[index];
    
    return NULL;
}

/* ****************************************************************************
 *  Removes a DNS question from a DNS question vector at a certain index
 * ****************************************************************************/
int
pico_dns_question_vector_remove( pico_dns_question_vector *vector,
                                 uint16_t index)
{
    struct pico_dns_question **new_questions = NULL;
    uint16_t i = 0;

    /* Check params */
    if (!vector) return -1;
    if (index >= vector->count) return -1;

    if (vector->count - 1u)
        new_questions = PICO_ZALLOC(sizeof(struct pico_dns_question *) *
                                    (vector->count - 1u));

    /* Move up subsequent records */
    for (i = index; i < (vector->count - 1); i++) {
        vector->questions[i] = vector->questions[i + 1];
        vector->questions[i + 1] = NULL;
    }
    vector->count--;

    /* Copy records */
    for (i = 0; i < vector->count; i++)
        new_questions[i] = vector->questions[i];

    /* Free the previous array */
    PICO_FREE(vector->questions);

    /* Set the records array to the new one */
    vector->questions = new_questions;
    return 0;
}

/* ****************************************************************************
 *  Deletes a DNS question from a DNS question vector at a certain index
 * ****************************************************************************/
int
pico_dns_question_vector_delete( pico_dns_question_vector *vector,
                                 uint16_t index)
{
    struct pico_dns_question **new_questions = NULL;
    uint16_t i = 0;
    
    /* Check params */
    if (!vector) return -1;
    if (index >= vector->count) return -1;
    
    /* Delete record */
    if (pico_dns_question_delete(&(vector->questions[index])) < 0)
        return -1;
    
    if (vector->count - 1u)
        new_questions = PICO_ZALLOC(sizeof(struct pico_dns_question *) *
                                   (vector->count - 1u));
    
    /* Move up subsequent records */
    for (i = index; i < (vector->count - 1); i++) {
        vector->questions[i] = vector->questions[i + 1];
        vector->questions[i + 1] = NULL;
    }
    vector->count--;
    
    /* Copy records */
    for (i = 0; i < vector->count; i++)
        new_questions[i] = vector->questions[i];
    
    /* Free the previous array */
    PICO_FREE(vector->questions);
    
    /* Set the records array to the new one */
    vector->questions = new_questions;
    return 0;
}

/* ****************************************************************************
 *  Deletes every DNS question from a DNS question vector
 * ****************************************************************************/
int
pico_dns_question_vector_destroy( pico_dns_question_vector *vector )
{
    uint16_t i = 0;
    
    /* Check params */
    if (!vector) return -1;
    
    /* Delete every record in the vector */
    for (i = 0; i < vector->count; i++) {
        if (pico_dns_question_delete(&(vector->questions[i])) < 0) {
            dns_dbg("Could not delete record from vector!\n");
            return -1;
        }
    }
    
    /* Update the fields */
    vector->questions = NULL;
    vector->count = 0;
    return 0;
}

/* ****************************************************************************
 *  Finds a DNS question in a DNS question vector
 * ****************************************************************************/
struct pico_dns_question *
pico_dns_question_vector_find_name( pico_dns_question_vector *vector,
                                    char *name )
{
    struct pico_dns_question *question = NULL;
    uint16_t i = 0;
    
    /* Check params */
    if (!vector || !name) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
    
    /* Iterate over the vector an compare names */
    for (i = 0; i < pico_dns_question_vector_count(vector); i++) {
        question = pico_dns_question_vector_get(vector, i);
        if (strcmp(question->qname, name) == 0)
            return question;
    }
    
    return NULL;
}

/* ****************************************************************************
 *  Deletes a DNS question from a DNS question vector
 * ****************************************************************************/
int
pico_dns_question_vector_del_name( pico_dns_question_vector *vector,
                                   char *name )
{
    struct pico_dns_question *question = NULL;
    uint16_t i = 0;

    /* Check params */
    if (!vector || !name) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    /* Iterate over the vector an compare names */
    for (i = 0; i < pico_dns_question_vector_count(vector); i++) {
        question = pico_dns_question_vector_get(vector, i);
        if (strcmp(question->qname, name) == 0) {
            if (pico_dns_question_vector_delete(vector, i) < 0) {
                dns_dbg("Could not delete question from probe cookie!\n");
                return -1;
            }
        }
    }

    return 0;
}

/* ****************************************************************************
 *  Returns the size in bytes of all the DNS questions contained in a DNS
 *  question-vector.
 * ****************************************************************************/
uint16_t
pico_dns_question_vector_size( pico_dns_question_vector *vector )
{
    struct pico_dns_question *question = NULL;
    uint16_t i = 0;
    size_t size = 0;
    
    /* Check params */
    if (!vector) {
        pico_err = PICO_ERR_EINVAL;
        return 0;
    }
    
    /* Add up the sizes */
    for (i = 0; i < pico_dns_question_vector_count(vector); i++) {
        question = pico_dns_question_vector_get(vector, i);
        size += (size_t)question->qname_length +
                sizeof(struct pico_dns_question_suffix);
    }
    return (uint16_t)size;
}

// MARK: QUERY FUNCTIONS

/* ****************************************************************************
 *  Creates a DNS packet meant for querying. Currently only questions can be
 *  inserted in the packet.
 * ****************************************************************************/
pico_dns_packet *
pico_dns_query_create( pico_dns_question_vector *qvector,
                       pico_dns_record_vector *anvector,
                       pico_dns_record_vector *nsvector,
                       pico_dns_record_vector *arvector,
                       uint16_t *len )
{
    pico_dns_packet *packet = NULL;
    pico_dns_question_vector _qvector = {0};
    pico_dns_record_vector _anvector = {0}, _nsvector = {0}, _arvector = {0};
    uint8_t qdcount = 0, ancount = 0, nscount = 0, arcount = 0;
    
    /* The length starts with the size of the header */
    *len = (uint16_t) sizeof(pico_dns_packet);
    
    if (qvector)
        _qvector = *qvector;
    if (anvector)
        _anvector = *anvector;
    if (nsvector)
        _nsvector = *nsvector;
    if (arvector)
        _arvector = *arvector;
    
    /* Get the size of the entire packet and determine the header counters */
    *len = (uint16_t)(*len + pico_dns_question_vector_size(&_qvector));
    qdcount = (uint8_t)pico_dns_question_vector_count(&_qvector);
    *len = (uint16_t)(*len + pico_dns_record_vector_size(&_anvector));
    ancount = (uint8_t)pico_dns_record_vector_count(&_anvector);
    *len = (uint16_t)(*len + pico_dns_record_vector_size(&_nsvector));
    nscount = (uint8_t)pico_dns_record_vector_count(&_nsvector);
    *len = (uint16_t)(*len + pico_dns_record_vector_size(&_arvector));
    arcount = (uint8_t)pico_dns_record_vector_count(&_arvector);
    
    /* Provide space for the entire packet */
    packet = PICO_ZALLOC(*len);
    if (!packet) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }

    /* Fill the Question Section with questions */
    if (pico_dns_fill_packet_question_section(packet, &_qvector)) {
        dns_dbg("Could not fill Question Section correctly!\n");
        return NULL;
    }

    /* Fill the Resource Record Sections with resource records */
    if (pico_dns_fill_packet_rr_sections(packet, &_qvector, &_anvector,
                                         &_nsvector, &_arvector)) {
        dns_dbg("Could not fill Resource Record Sections correctly!\n");
        return NULL;
    }

    /* Fill the DNS packet header */
    pico_dns_fill_packet_header(packet, qdcount, ancount, nscount, arcount);

    /* Apply DNS name compression */
    pico_dns_packet_compress(packet, len);

    return packet;
}

// MARK: RESOURCE RECORD FUNCTIONS

/* ****************************************************************************
 *  Fills the resource record fixed-sized flags & fields accordingly.
 * ****************************************************************************/
static void
pico_dns_record_fill_suffix( struct pico_dns_record_suffix *suf,
                             uint16_t rtype,
                             uint16_t rclass,
                             uint32_t rttl,
                             uint16_t rdlength )
{
    suf->rtype = short_be(rtype);
    suf->rclass = short_be(rclass);
    suf->rttl = long_be(rttl);
    suf->rdlength = short_be(rdlength);
}

/* ****************************************************************************
 *  Copies the contents a resource record [record] to a single flat
 *  location in [destination]. [destination] pointer will point to address
 *  right after this flat resource record on success.
 * ****************************************************************************/
static int
pico_dns_record_copy_flat( struct pico_dns_record *record,
                           uint8_t **destination )
{
    char *dest_rname = NULL; // rname destination location
    struct pico_dns_record_suffix *dest_rsuffix = NULL; // rsuffix destin.
    uint8_t *dest_rdata = NULL; // rdata destination location
    
    /* Check if there are no NULL-pointers given */
    if (!record || !destination || !(*destination)) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Initialise the destiation pointers to the right locations */
    dest_rname = (char *) *destination;
    dest_rsuffix = (struct pico_dns_record_suffix *)
    (dest_rname + record->rname_length);
    dest_rdata = ((uint8_t *)dest_rsuffix +
                  sizeof(struct pico_dns_record_suffix));
    
    /* Copy the rname of the resource record into the flat location */
    strcpy(dest_rname, record->rname);
    
    /* Copy the question suffix fields */
    dest_rsuffix->rtype = record->rsuffix->rtype;
    dest_rsuffix->rclass = record->rsuffix->rclass;
    dest_rsuffix->rttl = record->rsuffix->rttl;
    dest_rsuffix->rdlength = record->rsuffix->rdlength;
    
    /* Copy the rdata of the resource */
    memcpy(dest_rdata,
           record->rdata,
           short_be(dest_rsuffix->rdlength));
    
    /* Point to location right after flat resource record */
    *destination = (uint8_t *)(dest_rdata +
                               short_be(record->rsuffix->rdlength));
    
    return 0;
}

/* ****************************************************************************
 *  Just copies a resource record provided in [record]
 * ****************************************************************************/
struct pico_dns_record *
pico_dns_record_copy( struct pico_dns_record *record )
{
    struct pico_dns_record *copy = NULL;

    if (!record) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }

    copy = PICO_ZALLOC(sizeof(struct pico_dns_record));
    if (!copy) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    copy->rname = PICO_ZALLOC((size_t)record->rname_length);
    if (!(copy->rname)) {
        pico_err = PICO_ERR_ENOMEM;
        PICO_FREE(copy);
        return NULL;
    }
    strcpy(copy->rname, record->rname);
    copy->rname_length = record->rname_length;
    copy->rsuffix = PICO_ZALLOC(sizeof(struct pico_dns_record_suffix));
    if (!(copy->rsuffix)) {
        pico_err = PICO_ERR_ENOMEM;
        PICO_FREE(copy->rname);
        PICO_FREE(copy);
        return NULL;
    }
    copy->rsuffix->rtype = record->rsuffix->rtype;
    copy->rsuffix->rclass = record->rsuffix->rclass;
    copy->rsuffix->rttl = record->rsuffix->rttl;
    copy->rsuffix->rdlength = record->rsuffix->rdlength;
    copy->rdata = PICO_ZALLOC((size_t)short_be(record->rsuffix->rdlength));
    if (!(copy->rdata)) {
        pico_err = PICO_ERR_ENOMEM;
        PICO_FREE(copy->rsuffix);
        PICO_FREE(copy->rname);
        PICO_FREE(copy);
        return NULL;
    }
    memcpy(copy->rdata, record->rdata, short_be(record->rsuffix->rdlength));
    
    return copy;
}

/* ****************************************************************************
 *  Deletes & free's the memory for a certain dns resource record
 * ****************************************************************************/
int
pico_dns_record_delete( struct pico_dns_record **rr )
{
    if (!rr || !(*rr))
        return 0;
    
    if ((*rr)->rname)
        PICO_FREE((*rr)->rname);
    
    if ((*rr)->rsuffix)
        PICO_FREE((*rr)->rsuffix);
    
    if ((*rr)->rdata)
        PICO_FREE((*rr)->rdata);

    PICO_FREE((*rr));
    *rr = NULL;
    
    return 0;
}

/* ****************************************************************************
 * Creates a standalone DNS resource record for given 'url'. Fills the
 * 'len'-argument with the total length of the record.
 * ****************************************************************************/
struct pico_dns_record *
pico_dns_record_create( const char *url,
                        void *_rdata,
                        uint16_t *len,
                        uint16_t rtype,
                        uint16_t rclass,
                        uint32_t rttl )
{
    struct pico_dns_record *record = NULL;  /* record to return */
    uint16_t slen, datalen;                         /* some lenghts */
    
    /* Cast the void pointer to a char pointer */
    char *rdata = (char *)_rdata;
    
    /* Get length + 2 for .-prefix en trailing zero-byte */
    slen = (uint16_t)(pico_dns_client_strlen(url) + 2u);
    
    /* Determine the length of rdata */
    switch (rtype)
    {
        case PICO_DNS_TYPE_A: datalen = PICO_SIZE_IP4; break;
        case PICO_DNS_TYPE_AAAA: datalen = PICO_SIZE_IP6; break;
        case PICO_DNS_TYPE_PTR:
            datalen = (uint16_t)(pico_dns_client_strlen(rdata) + 2u);
            break;
        default:
            datalen = (uint16_t)(strlen(rdata));
            break;
    }
    
    /* Allocate space for the record and subfields */
    record = PICO_ZALLOC(sizeof(struct pico_dns_record));
    record->rname = PICO_ZALLOC(slen);
    record->rsuffix = PICO_ZALLOC(sizeof(struct pico_dns_record_suffix));
    record->rdata = PICO_ZALLOC(datalen);
    if (!record || !(record->rname) || !(record->rsuffix) || !(record->rdata)) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    
    /* Determine the complete length of resource record */
    *len = (uint16_t)(slen + sizeof(struct pico_dns_record_suffix) + datalen);
    
    /* Fill in the rname_length field */
    record->rname_length = (uint8_t)slen;
    
    /* Copy url into rname in DNS notation */
    strcpy(record->rname + 1u, url);
    pico_dns_name_to_dns_notation(record->rname);
    
    /* Fill in the resource record suffix */
    pico_dns_record_fill_suffix(record->rsuffix, rtype, rclass, rttl, datalen);
    
    /* Fill in the rdata */
    if (rtype != PICO_DNS_TYPE_PTR) {
        memcpy(record->rdata, rdata, datalen);
    } else {
        memcpy(record->rdata + 1u, rdata, strlen(rdata));
        pico_dns_name_to_dns_notation((char *)record->rdata);
    }

    return record;
}

/* ****************************************************************************
 *  Initialise an DNS record vector
 * ****************************************************************************/
int
pico_dns_record_vector_init( pico_dns_record_vector *vector )
{
    /* Check params */
    if (!vector) return -1;
    vector->records = NULL;
    vector->count = 0;
    return 0;
}

/* ****************************************************************************
 *  Returns the amount of records contained in the DNS record vector
 * ****************************************************************************/
uint16_t
pico_dns_record_vector_count( pico_dns_record_vector *vector )
{
    /* Check params */
    if (!vector) return 0;
    return vector->count;
}

/* ****************************************************************************
 *  Adds a DNS record to a DNS record vector
 * ****************************************************************************/
int
pico_dns_record_vector_add( pico_dns_record_vector *vector,
                            struct pico_dns_record *record )
{
    struct pico_dns_record **new_records = NULL;
    uint16_t i = 0;
    
    /* Check params */
    if (!vector || !record) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Create a new array with larger size */
    new_records = PICO_ZALLOC(sizeof(struct pico_dns_record *) *
                              (vector->count + 1u));
    if (!new_records)
        return -1;
    
    /* Copy all the record-pointers from the previous array to the new one */
    for (i = 0; i < vector->count; i++)
        new_records[i] = vector->records[i];
    new_records[i] = record;
    
    /* Free the previous array */
    if (vector->records)
        PICO_FREE(vector->records);

    /* Set the records array to the new one and update count */
    vector->records = new_records;
    vector->count++;
    return 0;
}

/* ****************************************************************************
 *  Adds a copy of a DNS record to a DNS record vector
 * ****************************************************************************/
int
pico_dns_record_vector_add_copy( pico_dns_record_vector *vector,
                                 struct pico_dns_record *record )
{
    struct pico_dns_record *copy = NULL;
    
    /* Check params */
    if (!vector || !record) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    
    /* Create copy */
    copy = pico_dns_record_copy(record);
    return pico_dns_record_vector_add(vector, copy);
}

/* ****************************************************************************
 *  Returns a DNS record from a DNS record vector at a certain index
 * ****************************************************************************/
struct pico_dns_record *
pico_dns_record_vector_get( pico_dns_record_vector *vector, uint16_t index)
{
    /* Check params */
    if (!vector)
        return NULL;
    
    /* Return record with conditioned index */
    if (index < vector->count)
        return vector->records[index];
    
    return NULL;
}

/* ****************************************************************************
 *  Deletes a DNS record from a DNS record vector at a certain index
 * ****************************************************************************/
int
pico_dns_record_vector_delete( pico_dns_record_vector *vector, uint16_t index)
{
    struct pico_dns_record **new_records = NULL;
    uint16_t i = 0;
    
    /* Check params */
    if (!vector) return -1;
    if (index >= vector->count) return -1;
    
    /* Delete record */
    if (pico_dns_record_delete(&(vector->records[index])) < 0)
        return -1;
    
    if (vector->count - 1u)
        new_records = PICO_ZALLOC(sizeof(struct pico_dns_record *) *
                                    (vector->count - 1u));
    
    /* Move up subsequent records */
    for (i = index; i < (vector->count - 1); i++) {
        vector->records[i] = vector->records[i + 1];
        vector->records[i + 1] = NULL;
    }
    vector->count--;
    
    /* Copy records */
    for (i = 0; i < vector->count; i++)
        new_records[i] = vector->records[i];
    
    /* Free the previous array */
    PICO_FREE(vector->records);
    
    /* Set the records array to the new one */
    vector->records = new_records;
    return 0;
}

/* ****************************************************************************
 *  Deletes every DNS record from a DNS record vector
 * ****************************************************************************/
int
pico_dns_record_vector_destroy( pico_dns_record_vector *vector )
{
    uint16_t i = 0;
    
    /* Check params */
    if (!vector) return -1;
    
    /* Delete every record in the vector */
    for (i = 0; i < vector->count; i++) {
        if (pico_dns_record_delete(&(vector->records[i])) < 0) {
            dns_dbg("Could not delete record from vector!\n");
            return -1;
        }
    }
    
    /* Update the fields */
    vector->records = NULL;
    vector->count = 0;
    return 0;
}

/* ****************************************************************************
 *  Returns the size in bytes of all the DNS records contained in a DNS
 *  record-vector.
 * ****************************************************************************/
uint16_t
pico_dns_record_vector_size( pico_dns_record_vector *vector )
{
    struct pico_dns_record *record = NULL;
    uint16_t i = 0;
    size_t size = 0;
    
    /* Check params */
    if (!vector) {
        pico_err = PICO_ERR_EINVAL;
        return 0;
    }
    
    /* Add up the sizes */
    for (i = 0; i < pico_dns_record_vector_count(vector); i++) {
        record = pico_dns_record_vector_get(vector, i);
        size = size + (size_t)record->rname_length +
               sizeof(struct pico_dns_record_suffix) +
               short_be(record->rsuffix->rdlength);
    }
    return (uint16_t)size;
}

// MARK: ANSWER FUNCTIONS

/* ****************************************************************************
 *  Creates a DNS Answer packet with given resource records to put in the
 *  Resource Record Sections. If a NULL-pointer is provided for a certain
 *  list, no records will be added to the packet for that section.
 * ****************************************************************************/
pico_dns_packet *
pico_dns_answer_create( pico_dns_record_vector *anvector,
                        pico_dns_record_vector *nsvector,
                        pico_dns_record_vector *arvector,
                        uint16_t *len )
{
    pico_dns_packet *packet = NULL;
    pico_dns_question_vector _qvector = {0};
    pico_dns_record_vector _anvector = {0}, _nsvector = {0}, _arvector = {0};
    uint8_t ancount = 0, nscount = 0, arcount = 0;

    if (anvector)
        _anvector = *anvector;
    if (nsvector)
        _anvector = *nsvector;
    if (arvector)
        _arvector = *arvector;
    
    /* The length start with the size of the header */
    *len = (uint16_t)sizeof(struct pico_dns_header);
    
    *len = (uint16_t)(*len + pico_dns_record_vector_size(&_anvector));
    ancount = (uint8_t)pico_dns_record_vector_count(&_anvector);
    *len = (uint16_t)(*len + pico_dns_record_vector_size(&_nsvector));
    nscount = (uint8_t)pico_dns_record_vector_count(&_nsvector);
    *len = (uint16_t)(*len + pico_dns_record_vector_size(&_arvector));
    arcount = (uint8_t)pico_dns_record_vector_count(&_arvector);
    
    /* Provide space for the entire packet */
    packet = PICO_ZALLOC(*len);
    if (!packet) {
        pico_err = PICO_ERR_ENOMEM;
        dns_dbg("Could not allocate memory for this packet!\n");
        return NULL;
    }
    
    /* Fill the resource record sections */
    if (pico_dns_fill_packet_rr_sections(packet, &_qvector, &_anvector,
                                         &_nsvector, &_arvector)) {
        dns_dbg("Could not fill Resource Record Sections correctly!\n");
        return NULL;
    }

    /* Fill the DNS packet header */
    pico_dns_fill_packet_header(packet, 0, ancount, nscount, arcount);
    
    /* Apply DNS name compression */
    pico_dns_packet_compress(packet, len);

    return packet;
}

// MARK: NAME & IP FUNCTIONS

/* ****************************************************************************
 *  Returns the length of an FQDN in a DNS-packet as if DNS name compression
 *  would be applied to the packet
 * ****************************************************************************/
uint16_t
pico_dns_namelen_comp( char *name )
{
    uint8_t *ptr = (uint8_t *)name; // Pointer to work with
    uint16_t len = 0;               // Length to return
    
    /* Just count until the zero-byte */
    while (*ptr != '\0' && !(*ptr & 0xC0)) {
        /* Move to next length-label */
        ptr += (uint8_t) *ptr + 1;
    }
    
    len = (uint16_t) (ptr - (uint8_t *)name);
    if(*ptr != '\0') {
        len++;
    }
    
    return len;
}

/* ****************************************************************************
 *  Returns the length of an FQDN. If DNS name compression is applied in the
 *  DNS packet, this will be the length as if the compressed name would be
 *  decompressed.
 * ****************************************************************************/
uint16_t
pico_dns_namelen_uncomp( char *name, pico_dns_packet *packet )
{
    uint8_t *begin = (uint8_t *)name;   // Stores the beginning of the name
    uint8_t *ptr = begin;               // Pointer to work with
    uint8_t *buf = NULL;                // DNS packet, byte addressable
    uint16_t comp_ptr = 0;              // Pointer in DNS packet index
    uint16_t len = 0;                   // Length to return
    
    /* Cast the DNS packet to a byte-addressable buffer */
    buf = (uint8_t *)packet;
    
    /* While we are not at the end of the name */
    while (*ptr != '\0') {
        /* Check if the first bit of the data is set - '|1|1|P|P|...|P|P|' */
        if(*ptr & 0xC0) {
            /* Move ptr to the pointer location */
            comp_ptr = (uint16_t)(((((uint16_t)*ptr ) << 8) & 0x3F00) | (uint16_t) *(ptr + 1));
            ptr = buf + comp_ptr;
        } else {
            /* Add the label length to the total length */
            len = (uint16_t)(len + (uint16_t)(*ptr & 0x3F) + 1);
            
            /* Move 'ptr' to the next length label */
            ptr += (*ptr + 1);
        }
    }
    return len;
}

/* ****************************************************************************
 *  Returns the uncompressed FQDN when DNS name compression is applied in the
 *  DNS packet.
 * ****************************************************************************/
char *
pico_dns_decompress_name( char *name, pico_dns_packet *packet )
{
    char *decompressed_name = NULL;
    uint8_t *dest_iterator = NULL;
    uint8_t *iterator = NULL;
    uint16_t ptr = 0;
    
    /* Provide storage for the uncompressed name */
    decompressed_name = PICO_ZALLOC((size_t)
                                    (pico_dns_namelen_uncomp(name,
                                                             packet)));
    if(!decompressed_name) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    
    /* Initialise iterators */
    iterator = (uint8_t *) name;
    dest_iterator = (uint8_t *) decompressed_name;
    while (*iterator != '\0') {
        if ((*iterator) & 0xC0) {
            /* We have a pointer */
            ptr = (uint16_t)((((uint16_t) *iterator) & 0x003F) << 8);
            ptr = (uint16_t)(ptr | (uint16_t) *(iterator + 1));
            iterator = (uint8_t *)((uint8_t *)packet + ptr);
        } else {
            /* We want to keep the label lengths */
            *dest_iterator = (uint8_t) *iterator;
            /* Copy the label */
            memcpy(dest_iterator + 1, iterator + 1, *iterator);
            /* Move to next length label */
            dest_iterator += (*iterator) + 1;
            iterator += (*iterator) + 1;
        }
    }
    
    /* Append final zero-byte */
    *dest_iterator = (uint8_t) '\0';
    
    return decompressed_name;
}

/* ****************************************************************************
 *  Gets the length of a given 'url' as if it where a qname for given qtype and
 *  protocol. Fills arpalen with the length of the arpa-suffix when qtype is
 *  PICO_DNS_TYPE_PTR, depending on [proto].
 * ****************************************************************************/
static uint16_t
pico_dns_url_get_reverse_len( const char *url,
                              uint16_t *arpalen,
                              uint16_t proto)
{
    uint16_t slen = 0;

    /* Check if pointers given are not NULL */
    if (!url && !arpalen) {
        pico_err = PICO_ERR_EINVAL;
        return 0;
    }
    /* Get length + 2 for .-prefix en trailing zero-byte by default */
    slen = (uint16_t)(pico_dns_client_strlen(url) + 2u);
    *arpalen = 0;

    /* Get the length of arpa-suffix if needed */
    if (proto == PICO_PROTO_IPV4)
        *arpalen = (uint16_t) strlen(PICO_ARPA_IPV4_SUFFIX);
#ifdef PICO_SUPPORT_IPV6
    else if (proto == PICO_PROTO_IPV6)
    {
        *arpalen = (uint16_t) strlen(PICO_ARPA_IPV6_SUFFIX);
        slen = STRLEN_PTR_IP6 + 2u;
    }
#endif
    return slen;
}

/* ****************************************************************************
 *  Returns the qname with [url] in DNS-format, with reverse resolving
 *  f.e.: www.google.com => 3www6google3com0
 * ****************************************************************************/
static char *
pico_dns_url_to_reverse_qname( const char *url, uint8_t proto )
{
    char *reverse_qname = NULL;
    uint16_t slen = 0, arpalen = 0;

    slen = pico_dns_url_get_reverse_len(url, &arpalen, proto);
    reverse_qname = PICO_ZALLOC((size_t)(slen + arpalen));
    if (!reverse_qname) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }

    /* If reverse IPv4 address resolving, convert to IPv4 arpa-format */
    if (proto == PICO_PROTO_IPV4) {
        memcpy(reverse_qname + 1u, url, strlen(url));
        pico_dns_mirror_addr(reverse_qname + 1u);
        memcpy(reverse_qname + (uint16_t)(strlen(url) + 2u) - 1,
               PICO_ARPA_IPV4_SUFFIX,
               strlen(PICO_ARPA_IPV4_SUFFIX));
        /* If reverse IPv6 address resolving, convert to IPv6 arpa-format */
    }
#ifdef PICO_SUPPORT_IPV6
    else if (proto == PICO_PROTO_IPV6) {
        pico_dns_ipv6_set_ptr(url, reverse_qname + 1u);
        memcpy(reverse_qname + 1u + STRLEN_PTR_IP6,
               PICO_ARPA_IPV6_SUFFIX,
               strlen(PICO_ARPA_IPV6_SUFFIX));
    }
#endif
    else {
        /* If you call this function you want a reverse qname */
    }
    pico_dns_name_to_dns_notation(reverse_qname);
    return reverse_qname;
}

/* ****************************************************************************
 *  Create an URL in *[url_addr] from any qname given in [qname]. [url_addr]
 *  needs to be an addres to a NULL-pointer. Returns a string
 *  1 byte smaller in size than [qname] or 2 bytes smaller than the
 *  string-length. Use PICO_FREE() to deallocate the memory for this pointer.
 *
 *  f.e. *  4tass5local0 -> tass.local
 *       *  11112102107in-addr4arpa0 -> 1.1.10.10.in-addr.arpa
 * ****************************************************************************/
char *
pico_dns_qname_to_url( const char *qname )
{
    char *url = NULL;   // URL-string to return
    char *temp = NULL;  // Temporary string
    
    /* Check if qname or url_addr is not a NULL-pointer */
    if (!qname) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
    
    /* Provide space for the url */
    url = PICO_ZALLOC(strlen(qname) - 2u);
    if (!url) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    
    /* Provide space for a temporary string to work with */
    temp = PICO_ZALLOC(strlen(qname) + 1u);
    if (!temp) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    
    /* Convert qname to an URL*/
    strcpy(temp, qname);
    pico_dns_notation_to_name(temp);
    strcpy(url, temp + 1);
    
    /* We don't need temp anymore, free memory */
    PICO_FREE(temp);
    
    return url;
}

/* ****************************************************************************
 * Create a qname in *[qname_addr] from any url given in [url]. [qname_addr]
 * needs to be an address to a NULL-pointer. Returns a string
 * 1 byte larger in size than [url] or 2 bytes larger than the
 * string-length. use PICO_FREE() to deallocate the memory for this pointer.
 *
 * f.e. -  tass.local -> 4tass5local0
 *      -  1.1.10.10.in-addr.arpa -> 11112102107in-addr4arpa0
 * ****************************************************************************/
char *
pico_dns_url_to_qname( const char *url )
{
    char *qname = NULL; // qname-string to return
    char *temp = NULL;  // temp string to work with
    
    /* Check if url or qname_addr is not a NULL-pointer */
    if (!url) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
    
    /* Provide space for the temporary string */
    temp = PICO_ZALLOC(strlen(url) + 1u);
    if (!temp) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    strcpy(temp, url);
    pico_to_lowercase(temp);
    
    /* Provide space for the qname */
    qname = PICO_ZALLOC(strlen(url) + 2u);
    if (!qname) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    
    /* Copy in the URL (+1 to leave space for leading '.') */
    strcpy(qname + 1, temp);
    
    /* Change to DNS notation */
    pico_dns_name_to_dns_notation(qname);
    
    return qname;
}

/* ****************************************************************************
 *  Determines the length of a string
 * ****************************************************************************/
uint16_t
pico_dns_client_strlen(const char *url)
{
    if (!url)
        return 0;
    return (uint16_t) strlen(url);
}

/* ****************************************************************************
 *  Converts a URL at location url + 1 to a FQDN in the form 3www6google3com0
 *  f.e. www.google.be => 3www6google2be0
 *  Size of ptr[] has to +2u more than the URL itself.
 * ****************************************************************************/
int
pico_dns_name_to_dns_notation(char *url)
{
    char p = 0, *label = NULL;
    uint8_t len = 0;

    if (!url)
        return -1;

    label = url++;
    while ((p = *url++) != 0) {
        if (p == '.') {
            *label = (char)len;
            label = url - 1;
            len = 0;
        } else {
            len++;
        }
    }
    *label = (char)len;
    return 0;
}

/* ****************************************************************************
 *  Converts a FQDN at location fqdn to an URL in the form .www.google.com
 *  f.e. 3www6google2be0 => .www.google.be
 * ****************************************************************************/
int
pico_dns_notation_to_name(char *fqdn)
{
    char p = 0, *label = NULL;

    if (!fqdn)
        return -1;

    label = fqdn;
    while ((p = *fqdn++) != 0) {
        fqdn += p;
        *label = '.';
        label = fqdn;
    }
    return 0;
}

/* ****************************************************************************
 *  mirror ip address numbers
 *  f.e. 192.168.0.1 => 1.0.168.192
 * ****************************************************************************/
int8_t
pico_dns_mirror_addr(char *ptr)
{
    const unsigned char *addr = NULL;
    char *m = ptr;
    uint32_t ip = 0;
    int8_t i = 0;

    if (pico_string_to_ipv4(ptr, &ip) < 0)
        return -1;

    ptr = m;
    addr = (unsigned char *)&ip;
    for (i = 3; i >= 0; i--) {
        if (addr[i] > 99) {
            *ptr++ = (char)('0' + (addr[i] / 100));
            *ptr++ = (char)('0' + ((addr[i] % 100) / 10));
            *ptr++ = (char)('0' + ((addr[i] % 100) % 10));
        } else if(addr[i] > 9) {
            *ptr++ = (char)('0' + (addr[i] / 10));
            *ptr++ = (char)('0' + (addr[i] % 10));
        } else {
            *ptr++ = (char)('0' + addr[i]);
        }

        if(i > 0)
            *ptr++ = '.';
    }
    *ptr = '\0';

    return 0;
}

#ifdef PICO_SUPPORT_IPV6
#define STRLEN_PTR_IP6 63

static inline char
dns_ptr_ip6_nibble_lo(uint8_t byte)
{
    uint8_t nibble = byte & 0x0f;
    if (nibble < 10)
        return (char)(nibble + '0');
    else
        return (char)(nibble - 0xa + 'a');
}

static inline char
dns_ptr_ip6_nibble_hi(uint8_t byte)
{
    uint8_t nibble = (byte & 0xf0u) >> 4u;
    if (nibble < 10u)
        return (char)(nibble + '0');
    else
        return (char)(nibble - 0xa + 'a');
}

void
pico_dns_ipv6_set_ptr(const char *ip, char *dst)
{
    /* Wow, initiasing struct fields, cool */
    struct pico_ip6 ip6 = {.addr = {}};
    int i, j = 0;
    pico_string_to_ipv6(ip, ip6.addr);
    for (i = 15; i >= 0; i--) {
        dst[j++] = dns_ptr_ip6_nibble_lo(ip6.addr[i]);
        dst[j++] = '.';
        dst[j++] = dns_ptr_ip6_nibble_hi(ip6.addr[i]);
        dst[j++] = '.';
    }
}
#endif