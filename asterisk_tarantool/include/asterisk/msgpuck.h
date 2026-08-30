/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2026, Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 *
 * Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 * \brief Lightweight MessagePack encoder and decoder for Asterisk Tarantool connector
 * \author Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 */

#ifndef _ASTERISK_MSGPUCK_H
#define _ASTERISK_MSGPUCK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* MP Constants */
#define MP_FIXINT_MAX 127
#define MP_FIXMAP 0x80
#define MP_FIXARRAY 0x90
#define MP_FIXSTR 0xa0
#define MP_NIL 0xc0
#define MP_FALSE 0xc2
#define MP_TRUE 0xc3
#define MP_BIN8 0xc4
#define MP_BIN16 0xc5
#define MP_BIN32 0xc6
#define MP_UINT8 0xcc
#define MP_UINT16 0xcd
#define MP_UINT32 0xce
#define MP_UINT64 0xcf
#define MP_INT8 0xd0
#define MP_INT16 0xd1
#define MP_INT32 0xd2
#define MP_INT64 0xd3
#define MP_STR8 0xd9
#define MP_STR16 0xda
#define MP_STR32 0xdb
#define MP_ARRAY16 0xdc
#define MP_ARRAY32 0xdd
#define MP_MAP16 0xde
#define MP_MAP32 0xdf

static inline char *mp_encode_nil(char *data)
{
	*data++ = (char)MP_NIL;
	return data;
}

static inline char *mp_encode_bool(char *data, bool val)
{
	*data++ = val ? (char)MP_TRUE : (char)MP_FALSE;
	return data;
}

static inline char *mp_encode_uint(char *data, uint64_t num)
{
	if (num <= 0x7f) {
		*data++ = (char)num;
	} else if (num <= 0xff) {
		*data++ = (char)MP_UINT8;
		*data++ = (char)num;
	} else if (num <= 0xffff) {
		*data++ = (char)MP_UINT16;
		*data++ = (char)(num >> 8);
		*data++ = (char)num;
	} else if (num <= 0xffffffff) {
		*data++ = (char)MP_UINT32;
		*data++ = (char)(num >> 24);
		*data++ = (char)(num >> 16);
		*data++ = (char)(num >> 8);
		*data++ = (char)num;
	} else {
		*data++ = (char)MP_UINT64;
		*data++ = (char)(num >> 56);
		*data++ = (char)(num >> 48);
		*data++ = (char)(num >> 40);
		*data++ = (char)(num >> 32);
		*data++ = (char)(num >> 24);
		*data++ = (char)(num >> 16);
		*data++ = (char)(num >> 8);
		*data++ = (char)num;
	}
	return data;
}

static inline char *mp_encode_int(char *data, int64_t num)
{
	if (num >= 0) {
		return mp_encode_uint(data, (uint64_t)num);
	} else if (num >= -32) {
		*data++ = (char)(0xe0 | (num + 32));
	} else if (num >= -128) {
		*data++ = (char)MP_INT8;
		*data++ = (char)num;
	} else if (num >= -32768) {
		*data++ = (char)MP_INT16;
		*data++ = (char)(num >> 8);
		*data++ = (char)num;
	} else if (num >= -2147483648LL) {
		*data++ = (char)MP_INT32;
		*data++ = (char)(num >> 24);
		*data++ = (char)(num >> 16);
		*data++ = (char)(num >> 8);
		*data++ = (char)num;
	} else {
		*data++ = (char)MP_INT64;
		*data++ = (char)(num >> 56);
		*data++ = (char)(num >> 48);
		*data++ = (char)(num >> 40);
		*data++ = (char)(num >> 32);
		*data++ = (char)(num >> 24);
		*data++ = (char)(num >> 16);
		*data++ = (char)(num >> 8);
		*data++ = (char)num;
	}
	return data;
}

static inline char *mp_encode_str(char *data, const char *str, uint32_t len)
{
	if (len <= 31) {
		*data++ = (char)(MP_FIXSTR | len);
	} else if (len <= 0xff) {
		*data++ = (char)MP_STR8;
		*data++ = (char)len;
	} else if (len <= 0xffff) {
		*data++ = (char)MP_STR16;
		*data++ = (char)(len >> 8);
		*data++ = (char)len;
	} else {
		*data++ = (char)MP_STR32;
		*data++ = (char)(len >> 24);
		*data++ = (char)(len >> 16);
		*data++ = (char)(len >> 8);
		*data++ = (char)len;
	}
	if (len > 0 && str != NULL) {
		memcpy(data, str, len);
		data += len;
	}
	return data;
}

static inline char *mp_encode_bin(char *data, const char *bin, uint32_t len)
{
	if (len <= 0xff) {
		*data++ = (char)MP_BIN8;
		*data++ = (char)len;
	} else if (len <= 0xffff) {
		*data++ = (char)MP_BIN16;
		*data++ = (char)(len >> 8);
		*data++ = (char)len;
	} else {
		*data++ = (char)MP_BIN32;
		*data++ = (char)(len >> 24);
		*data++ = (char)(len >> 16);
		*data++ = (char)(len >> 8);
		*data++ = (char)len;
	}
	if (len > 0 && bin != NULL) {
		memcpy(data, bin, len);
		data += len;
	}
	return data;
}

static inline char *mp_encode_array(char *data, uint32_t size)
{
	if (size <= 15) {
		*data++ = (char)(MP_FIXARRAY | size);
	} else if (size <= 0xffff) {
		*data++ = (char)MP_ARRAY16;
		*data++ = (char)(size >> 8);
		*data++ = (char)size;
	} else {
		*data++ = (char)MP_ARRAY32;
		*data++ = (char)(size >> 24);
		*data++ = (char)(size >> 16);
		*data++ = (char)(size >> 8);
		*data++ = (char)size;
	}
	return data;
}

static inline char *mp_encode_map(char *data, uint32_t size)
{
	if (size <= 15) {
		*data++ = (char)(MP_FIXMAP | size);
	} else if (size <= 0xffff) {
		*data++ = (char)MP_MAP16;
		*data++ = (char)(size >> 8);
		*data++ = (char)size;
	} else {
		*data++ = (char)MP_MAP32;
		*data++ = (char)(size >> 24);
		*data++ = (char)(size >> 16);
		*data++ = (char)(size >> 8);
		*data++ = (char)size;
	}
	return data;
}

static inline uint32_t mp_decode_uint(const char **data)
{
	const uint8_t *c = (const uint8_t *)*data;
	uint8_t tag = *c++;
	if (tag <= 0x7f) {
		*data = (const char *)c;
		return tag;
	}
	if (tag == MP_UINT8) {
		uint32_t val = *c++;
		*data = (const char *)c;
		return val;
	}
	if (tag == MP_UINT16) {
		uint32_t val = ((uint32_t)c[0] << 8) | c[1];
		*data = (const char *)(c + 2);
		return val;
	}
	if (tag == MP_UINT32) {
		uint32_t val = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
		*data = (const char *)(c + 4);
		return val;
	}
	*data = (const char *)c;
	return 0;
}

static inline const char *mp_decode_str(const char **data, uint32_t *len)
{
	const uint8_t *c = (const uint8_t *)*data;
	uint8_t tag = *c++;
	if ((tag & 0xe0) == MP_FIXSTR) {
		*len = tag & 0x1f;
		const char *s = (const char *)c;
		*data = (const char *)(c + *len);
		return s;
	}
	if (tag == MP_STR8) {
		*len = *c++;
		const char *s = (const char *)c;
		*data = (const char *)(c + *len);
		return s;
	}
	if (tag == MP_STR16) {
		*len = ((uint32_t)c[0] << 8) | c[1];
		c += 2;
		const char *s = (const char *)c;
		*data = (const char *)(c + *len);
		return s;
	}
	if (tag == MP_STR32) {
		*len = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
		c += 4;
		const char *s = (const char *)c;
		*data = (const char *)(c + *len);
		return s;
	}
	*len = 0;
	*data = (const char *)c;
	return NULL;
}

static inline uint32_t mp_decode_array(const char **data)
{
	const uint8_t *c = (const uint8_t *)*data;
	uint8_t tag = *c++;
	if ((tag & 0xf0) == MP_FIXARRAY) {
		*data = (const char *)c;
		return tag & 0x0f;
	}
	if (tag == MP_ARRAY16) {
		uint32_t len = ((uint32_t)c[0] << 8) | c[1];
		*data = (const char *)(c + 2);
		return len;
	}
	if (tag == MP_ARRAY32) {
		uint32_t len = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
		*data = (const char *)(c + 4);
		return len;
	}
	*data = (const char *)c;
	return 0;
}

static inline uint32_t mp_decode_map(const char **data)
{
	const uint8_t *c = (const uint8_t *)*data;
	uint8_t tag = *c++;
	if ((tag & 0xf0) == MP_FIXMAP) {
		*data = (const char *)c;
		return tag & 0x0f;
	}
	if (tag == MP_MAP16) {
		uint32_t len = ((uint32_t)c[0] << 8) | c[1];
		*data = (const char *)(c + 2);
		return len;
	}
	if (tag == MP_MAP32) {
		uint32_t len = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
		*data = (const char *)(c + 4);
		return len;
	}
	*data = (const char *)c;
	return 0;
}

#if defined(__cplusplus)
}
#endif

#endif /* _ASTERISK_MSGPUCK_H */
