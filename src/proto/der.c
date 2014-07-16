// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
/* Copyright 2014, SecurActive.
 *
 * This file is part of Junkie.
 *
 * Junkie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Junkie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Junkie.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "junkie/proto/der.h"
#include "junkie/tools/tempstr.h"
#include "junkie/tools/log.h"
#include <inttypes.h>

static char *der_class_identifier_2_str(enum der_class_identifier der_class_identifier)
{
    switch (der_class_identifier) {
        case DER_UNIVERSAL        : return "DER_UNIVERSAL";
        case DER_APPLICATION      : return "DER_APPLICATION";
        case DER_CONTEXT_SPECIFIC : return "DER_CONTEXT_SPECIFIC";
        case DER_PRIVATE          : return "DER_PRIVATE";
        default                   : return tempstr_printf("Unknown (0x%"PRIx32")", der_class_identifier);
    }
}

static char *der_type_2_str(enum der_type der_type)
{
    switch (der_type) {
        case DER_PRIMITIVE        : return "DER_PRIMITIVE";
        case DER_CONSTRUCTED      : return "DER_CONSTRUCTED";
        default                   : return tempstr_printf("Unknown (0x%"PRIx32")", der_type);
    }
}

static char *der_class_tag_2_str(enum der_class_tag der_class_tag)
{
    switch (der_class_tag) {
        case DER_EOC               : return "DER_EOC";
        case DER_BOOLEAN           : return "DER_BOOLEAN";
        case DER_INTEGER           : return "DER_INTEGER";
        case DER_BIT_STRING        : return "DER_BIT_STRING";
        case DER_OCTET_STRING      : return "DER_OCTET_STRING";
        case DER_NULL              : return "DER_NULL";
        case DER_OBJECT_IDENTIFIER : return "DER_OBJECT_IDENTIFIER";
        case DER_OBJECT_DESCRIPTOR : return "DER_OBJECT_DESCRIPTOR";
        case DER_EXTERNAL          : return "DER_EXTERNAL";
        case DER_REAL              : return "DER_REAL";
        case DER_ENUMERATED        : return "DER_ENUMERATED";
        case DER_EMBEDDED_PDV      : return "DER_EMBEDDED_PDV";
        case DER_UTF8STRING        : return "DER_UTF8STRING";
        case DER_RELATIVE_OID      : return "DER_RELATIVE_OID";
        case DER_SEQUENCE          : return "DER_SEQUENCE";
        case DER_SET               : return "DER_SET";
        case DER_NUMERIC_STRING    : return "DER_NUMERIC_STRING";
        case DER_PRINTABLE_STRING  : return "DER_PRINTABLE_STRING";
        case DER_T61_STRING        : return "DER_T61_STRING";
        case DER_VIDEOTEX_STRING   : return "DER_VIDEOTEX_STRING";
        case DER_IA5_STRING        : return "DER_IA5_STRING";
        case DER_UTC_TIME          : return "DER_UTC_TIME";
        case DER_GENERALIZED_TIME  : return "DER_GENERALIZED_TIME";
        case DER_GRAPHIC_STRING    : return "DER_GRAPHIC_STRING";
        case DER_VISIBLE_STRING    : return "DER_VISIBLE_STRING";
        case DER_GENERAL_STRING    : return "DER_GENERAL_STRING";
        case DER_UNIVERSAL_STRING  : return "DER_UNIVERSAL_STRING";
        case DER_CHARACTER_STRING  : return "DER_CHARACTER_STRING";
        case DER_BMP_STRING        : return "DER_BMP_STRING";
        case DER_LONG_FORM         : return "DER_LONG_FORM";
        default                    : return tempstr_printf("Unknown (0x%"PRIx32")", der_class_tag);
    }
}

char *der_2_str(struct der *der)
{
    char *str = tempstr_printf("Der class %s, type: %s, class tag: %s, size %"PRIu64,
            der_class_identifier_2_str(der->class_identifier),
            der_type_2_str(der->type),
            der_class_tag_2_str(der->class_tag),
            der->length);
    return str;
}

#define DER_CLASS_IDENTIFIER 0b11000000
#define DER_TYPE 0b00100000
#define DER_CLASS_TAG 0b00011111

#define DER_LEFT_BIT_MASK 0x80

/**
 * Short form. One octet. Bit 8 has value "0" and bits 7-1 give the length.
 *
 * Long form. Two to 127 octets. Bit 8 of first octet has value "1" and bits 7-1 give the number of additional length octets. Second and following octets give the length, base 256, most significant digit first.
 */
static enum proto_parse_status cursor_read_der_length(struct cursor *cursor, uint_least64_t *out_res)
{
    uint8_t current = cursor_read_u8(cursor);
    if (current & DER_LEFT_BIT_MASK) {
        uint16_t num_bytes = current & ~DER_LEFT_BIT_MASK;
        if (num_bytes > cursor->cap_len) return 0;
        return cursor_read_fixed_int_n(cursor, out_res, num_bytes);
    } else {
        *out_res = current;
        return PROTO_OK;
    }
}

enum proto_parse_status cursor_read_der(struct cursor *cursor, struct der *der)
{
    enum proto_parse_status status = PROTO_OK;
    uint8_t der_header = cursor_read_u8(cursor);
    der->class_identifier = (der_header & DER_CLASS_IDENTIFIER) >> 6;
    der->type = (der_header & DER_TYPE) >> 5;
    der->class_tag = der_header & DER_CLASS_TAG;
    if (PROTO_OK != (status = cursor_read_der_length(cursor, &der->length))) return status;
    SLOG(LOG_DEBUG, "Parsed der %s, %"PRIx16, der_2_str(der), der_header);
    if (der->length > cursor->cap_len) return PROTO_TOO_SHORT;
    der->value = cursor->head;
    return PROTO_OK;
}

/**
 * Node values less than or equal to 127 are encoded on one byte.
 * Node values greater than or equal to 128 are encoded on multiple bytes. Bit 7 of the leftmost byte is set to one. Bits 0 through 6 of each byte contains the encoded value.
 */
static uint16_t cursor_read_oid_node(struct cursor *cursor)
{
    uint8_t current = cursor_read_u8(cursor);
    if (current & DER_LEFT_BIT_MASK) {
        uint16_t left = (current & ~DER_LEFT_BIT_MASK) << 7;
        uint8_t right = cursor_read_u8(cursor) & ~DER_LEFT_BIT_MASK;
        SLOG(LOG_DEBUG, "Got a multibyte length, left part %"PRIu16", right part %"PRIu8, left, right);
        return left + right;
    } else {
        return current;
    }
}

enum proto_parse_status cursor_read_oid(struct cursor *cursor, size_t size, uint16_t *oid, size_t *oid_length)
{
    assert(oid);
    if (cursor->cap_len < size) return PROTO_TOO_SHORT;

    // The first two nodes are encoded on a single byte.
    // The first node is multiplied by the decimal 40 and the result is added to the value of the second node.
    int oid_indice = 0;
    uint8_t first_byte = cursor_read_u8(cursor);
    oid[oid_indice++] = first_byte / 40;
    oid[oid_indice++] = first_byte - oid[0] * 40;

    for (unsigned i = 1; i < size; i++) {
         uint16_t node = cursor_read_oid_node(cursor);
         if (node > 127) i++;
         oid[oid_indice++] = node;
    }
    if (oid_length) *oid_length = oid_indice;
    return PROTO_OK;
}
