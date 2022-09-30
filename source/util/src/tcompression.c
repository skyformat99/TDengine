/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* README.md   TAOS compression
 *
 * INTEGER Compression Algorithm:
 *   To compress integers (including char, short, int32_t, int64_t), the difference
 *   between two integers is calculated at first. Then the difference is
 *   transformed to positive by zig-zag encoding method
 *   (https://gist.github.com/mfuerstenau/ba870a29e16536fdbaba). Then the value is
 *   encoded using simple 8B method. For more information about simple 8B,
 *   refer to https://en.wikipedia.org/wiki/8b/10b_encoding.
 *
 *   NOTE : For bigint, only 59 bits can be used, which means data from -(2**59) to (2**59)-1
 *   are allowed.
 *
 * BOOLEAN Compression Algorithm:
 *   We provide two methods for compress boolean types. Because boolean types in C
 *   code are char bytes with 0 and 1 values only, only one bit can used to discriminate
 *   the values.
 *   1. The first method is using only 1 bit to represent the boolean value with 1 for
 *   true and 0 for false. Then the compression rate is 1/8.
 *   2. The second method is using run length encoding (RLE) methods. This method works
 *   better when there are a lot of consecutive true values or false values.
 *
 * STRING Compression Algorithm:
 *   We us LZ4 method to compress the string type.
 *
 * FLOAT Compression Algorithm:
 *   We use the same method with Akumuli to compress float and double types. The compression
 *   algorithm assumes the float/double values change slightly. So we take the XOR between two
 *   adjacent values. Then compare the number of leading zeros and trailing zeros. If the number
 *   of leading zeros are larger than the trailing zeros, then record the last serveral bytes
 *   of the XORed value with informations. If not, record the first corresponding bytes.
 *
 */

#define _DEFAULT_SOURCE
#include "tcompression.h"
#include "lz4.h"
#include "tRealloc.h"
#include "tlog.h"

#ifdef TD_TSZ
#include "td_sz.h"
#endif

static const int32_t TEST_NUMBER = 1;
#define is_bigendian()     ((*(char *)&TEST_NUMBER) == 0)
#define SIMPLE8B_MAX_INT64 ((uint64_t)1152921504606846974LL)

#define safeInt64Add(a, b)  (((a >= 0) && (b <= INT64_MAX - a)) || ((a < 0) && (b >= INT64_MIN - a)))
#define ZIGZAG_ENCODE(T, v) ((u##T)((v) >> (sizeof(T) * 8 - 1))) ^ (((u##T)(v)) << 1)  // zigzag encode
#define ZIGZAG_DECODE(T, v) ((v) >> 1) ^ -((T)((v)&1))                                 // zigzag decode

#ifdef TD_TSZ
bool lossyFloat = false;
bool lossyDouble = false;

// init call
int32_t tsCompressInit() {
  // config
  if (lossyColumns[0] == 0) {
    lossyFloat = false;
    lossyDouble = false;
    return 0;
  }

  lossyFloat = strstr(lossyColumns, "float") != NULL;
  lossyDouble = strstr(lossyColumns, "double") != NULL;

  if (lossyFloat == false && lossyDouble == false) return 0;

  tdszInit(fPrecision, dPrecision, maxRange, curRange, Compressor);
  if (lossyFloat) uTrace("lossy compression float  is opened. ");
  if (lossyDouble) uTrace("lossy compression double is opened. ");
  return 1;
}
// exit call
void tsCompressExit() { tdszExit(); }

#endif

/*
 * Compress Integer (Simple8B).
 */
int32_t tsCompressINTImp(const char *const input, const int32_t nelements, char *const output, const char type) {
  // Selector value:              0    1   2   3   4   5   6   7   8  9  10  11
  // 12  13  14  15
  char    bit_per_integer[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60};
  int32_t selector_to_elems[] = {240, 120, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1};
  char    bit_to_selector[] = {0,  2,  3,  4,  5,  6,  7,  8,  9,  10, 10, 11, 11, 12, 12, 12, 13, 13, 13, 13, 13,
                               14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                               15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15};

  // get the byte limit.
  int32_t word_length = 0;
  switch (type) {
    case TSDB_DATA_TYPE_BIGINT:
      word_length = LONG_BYTES;
      break;
    case TSDB_DATA_TYPE_INT:
      word_length = INT_BYTES;
      break;
    case TSDB_DATA_TYPE_SMALLINT:
      word_length = SHORT_BYTES;
      break;
    case TSDB_DATA_TYPE_TINYINT:
      word_length = CHAR_BYTES;
      break;
    default:
      uError("Invalid compress integer type:%d", type);
      return -1;
  }

  int32_t byte_limit = nelements * word_length + 1;
  int32_t opos = 1;
  int64_t prev_value = 0;

  for (int32_t i = 0; i < nelements;) {
    char    selector = 0;
    char    bit = 0;
    int32_t elems = 0;
    int64_t prev_value_tmp = prev_value;

    for (int32_t j = i; j < nelements; j++) {
      // Read data from the input stream and convert it to INT64 type.
      int64_t curr_value = 0;
      switch (type) {
        case TSDB_DATA_TYPE_TINYINT:
          curr_value = (int64_t)(*((int8_t *)input + j));
          break;
        case TSDB_DATA_TYPE_SMALLINT:
          curr_value = (int64_t)(*((int16_t *)input + j));
          break;
        case TSDB_DATA_TYPE_INT:
          curr_value = (int64_t)(*((int32_t *)input + j));
          break;
        case TSDB_DATA_TYPE_BIGINT:
          curr_value = (int64_t)(*((int64_t *)input + j));
          break;
      }
      // Get difference.
      if (!safeInt64Add(curr_value, -prev_value_tmp)) goto _copy_and_exit;

      int64_t diff = curr_value - prev_value_tmp;
      // Zigzag encode the value.
      uint64_t zigzag_value = ZIGZAG_ENCODE(int64_t, diff);

      if (zigzag_value >= SIMPLE8B_MAX_INT64) goto _copy_and_exit;

      int64_t tmp_bit;
      if (zigzag_value == 0) {
        // Take care here, __builtin_clzl give wrong anser for value 0;
        tmp_bit = 0;
      } else {
        tmp_bit = (LONG_BYTES * BITS_PER_BYTE) - BUILDIN_CLZL(zigzag_value);
      }

      if (elems + 1 <= selector_to_elems[(int32_t)selector] &&
          elems + 1 <= selector_to_elems[(int32_t)(bit_to_selector[(int32_t)tmp_bit])]) {
        // If can hold another one.
        selector = selector > bit_to_selector[(int32_t)tmp_bit] ? selector : bit_to_selector[(int32_t)tmp_bit];
        elems++;
        bit = bit_per_integer[(int32_t)selector];
      } else {
        // if cannot hold another one.
        while (elems < selector_to_elems[(int32_t)selector]) selector++;
        elems = selector_to_elems[(int32_t)selector];
        bit = bit_per_integer[(int32_t)selector];
        break;
      }
      prev_value_tmp = curr_value;
    }

    uint64_t buffer = 0;
    buffer |= (uint64_t)selector;
    for (int32_t k = 0; k < elems; k++) {
      int64_t curr_value = 0; /* get current values */
      switch (type) {
        case TSDB_DATA_TYPE_TINYINT:
          curr_value = (int64_t)(*((int8_t *)input + i));
          break;
        case TSDB_DATA_TYPE_SMALLINT:
          curr_value = (int64_t)(*((int16_t *)input + i));
          break;
        case TSDB_DATA_TYPE_INT:
          curr_value = (int64_t)(*((int32_t *)input + i));
          break;
        case TSDB_DATA_TYPE_BIGINT:
          curr_value = (int64_t)(*((int64_t *)input + i));
          break;
      }
      int64_t  diff = curr_value - prev_value;
      uint64_t zigzag_value = ZIGZAG_ENCODE(int64_t, diff);
      buffer |= ((zigzag_value & INT64MASK(bit)) << (bit * k + 4));
      i++;
      prev_value = curr_value;
    }

    // Output the encoded value to the output.
    if (opos + sizeof(buffer) <= byte_limit) {
      memcpy(output + opos, &buffer, sizeof(buffer));
      opos += sizeof(buffer);
    } else {
    _copy_and_exit:
      output[0] = 1;
      memcpy(output + 1, input, byte_limit - 1);
      return byte_limit;
    }
  }

  // set the indicator.
  output[0] = 0;
  return opos;
}

int32_t tsDecompressINTImp(const char *const input, const int32_t nelements, char *const output, const char type) {
  int32_t word_length = 0;
  switch (type) {
    case TSDB_DATA_TYPE_BIGINT:
      word_length = LONG_BYTES;
      break;
    case TSDB_DATA_TYPE_INT:
      word_length = INT_BYTES;
      break;
    case TSDB_DATA_TYPE_SMALLINT:
      word_length = SHORT_BYTES;
      break;
    case TSDB_DATA_TYPE_TINYINT:
      word_length = CHAR_BYTES;
      break;
    default:
      uError("Invalid decompress integer type:%d", type);
      return -1;
  }

  // If not compressed.
  if (input[0] == 1) {
    memcpy(output, input + 1, nelements * word_length);
    return nelements * word_length;
  }

  // Selector value:              0    1   2   3   4   5   6   7   8  9  10  11
  // 12  13  14  15
  char    bit_per_integer[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60};
  int32_t selector_to_elems[] = {240, 120, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1};

  const char *ip = input + 1;
  int32_t     count = 0;
  int32_t     _pos = 0;
  int64_t     prev_value = 0;

  while (1) {
    if (count == nelements) break;

    uint64_t w = 0;
    memcpy(&w, ip, LONG_BYTES);

    char    selector = (char)(w & INT64MASK(4));       // selector = 4
    char    bit = bit_per_integer[(int32_t)selector];  // bit = 3
    int32_t elems = selector_to_elems[(int32_t)selector];

    for (int32_t i = 0; i < elems; i++) {
      uint64_t zigzag_value;

      if (selector == 0 || selector == 1) {
        zigzag_value = 0;
      } else {
        zigzag_value = ((w >> (4 + bit * i)) & INT64MASK(bit));
      }
      int64_t diff = ZIGZAG_DECODE(int64_t, zigzag_value);
      int64_t curr_value = diff + prev_value;
      prev_value = curr_value;

      switch (type) {
        case TSDB_DATA_TYPE_BIGINT:
          *((int64_t *)output + _pos) = (int64_t)curr_value;
          _pos++;
          break;
        case TSDB_DATA_TYPE_INT:
          *((int32_t *)output + _pos) = (int32_t)curr_value;
          _pos++;
          break;
        case TSDB_DATA_TYPE_SMALLINT:
          *((int16_t *)output + _pos) = (int16_t)curr_value;
          _pos++;
          break;
        case TSDB_DATA_TYPE_TINYINT:
          *((int8_t *)output + _pos) = (int8_t)curr_value;
          _pos++;
          break;
        default:
          perror("Wrong integer types.\n");
          return -1;
      }
      count++;
      if (count == nelements) break;
    }
    ip += LONG_BYTES;
  }

  return nelements * word_length;
}

/* ----------------------------------------------Bool Compression
 * ---------------------------------------------- */
// TODO: You can also implement it using RLE method.
int32_t tsCompressBoolImp(const char *const input, const int32_t nelements, char *const output) {
  int32_t pos = -1;
  int32_t ele_per_byte = BITS_PER_BYTE / 2;

  for (int32_t i = 0; i < nelements; i++) {
    if (i % ele_per_byte == 0) {
      pos++;
      output[pos] = 0;
    }

    uint8_t t = 0;
    if (input[i] == 1) {
      t = (((uint8_t)1) << (2 * (i % ele_per_byte)));
      output[pos] |= t;
    } else if (input[i] == 0) {
      t = ((uint8_t)1 << (2 * (i % ele_per_byte))) - 1;
      /* t = (~((( uint8_t)1) << (7-i%BITS_PER_BYTE))); */
      output[pos] &= t;
    } else if (input[i] == TSDB_DATA_BOOL_NULL) {
      t = ((uint8_t)2 << (2 * (i % ele_per_byte)));
      /* t = (~((( uint8_t)1) << (7-i%BITS_PER_BYTE))); */
      output[pos] |= t;
    } else {
      uError("Invalid compress bool value:%d", output[pos]);
      return -1;
    }
  }

  return pos + 1;
}

int32_t tsDecompressBoolImp(const char *const input, const int32_t nelements, char *const output) {
  int32_t ipos = -1, opos = 0;
  int32_t ele_per_byte = BITS_PER_BYTE / 2;

  for (int32_t i = 0; i < nelements; i++) {
    if (i % ele_per_byte == 0) {
      ipos++;
    }

    uint8_t ele = (input[ipos] >> (2 * (i % ele_per_byte))) & INT8MASK(2);
    if (ele == 1) {
      output[opos++] = 1;
    } else if (ele == 2) {
      output[opos++] = TSDB_DATA_BOOL_NULL;
    } else {
      output[opos++] = 0;
    }
  }

  return nelements;
}

/* Run Length Encoding(RLE) Method */
int32_t tsCompressBoolRLEImp(const char *const input, const int32_t nelements, char *const output) {
  int32_t _pos = 0;

  for (int32_t i = 0; i < nelements;) {
    unsigned char counter = 1;
    char          num = input[i];

    for (++i; i < nelements; i++) {
      if (input[i] == num) {
        counter++;
        if (counter == INT8MASK(7)) {
          i++;
          break;
        }
      } else {
        break;
      }
    }

    // Encode the data.
    if (num == 1) {
      output[_pos++] = INT8MASK(1) | (counter << 1);
    } else if (num == 0) {
      output[_pos++] = (counter << 1) | INT8MASK(0);
    } else {
      uError("Invalid compress bool value:%d", output[_pos]);
      return -1;
    }
  }

  return _pos;
}

int32_t tsDecompressBoolRLEImp(const char *const input, const int32_t nelements, char *const output) {
  int32_t ipos = 0, opos = 0;
  while (1) {
    char     encode = input[ipos++];
    unsigned counter = (encode >> 1) & INT8MASK(7);
    char     value = encode & INT8MASK(1);

    memset(output + opos, value, counter);
    opos += counter;
    if (opos >= nelements) {
      return nelements;
    }
  }
}

/* ----------------------------------------------String Compression
 * ---------------------------------------------- */
// Note: the size of the output must be larger than input_size + 1 and
// LZ4_compressBound(size) + 1;
// >= max(input_size, LZ4_compressBound(input_size)) + 1;
int32_t tsCompressStringImp(const char *const input, int32_t inputSize, char *const output, int32_t outputSize) {
  // Try to compress using LZ4 algorithm.
  const int32_t compressed_data_size = LZ4_compress_default(input, output + 1, inputSize, outputSize - 1);

  // If cannot compress or after compression, data becomes larger.
  if (compressed_data_size <= 0 || compressed_data_size > inputSize) {
    /* First byte is for indicator */
    output[0] = 0;
    memcpy(output + 1, input, inputSize);
    return inputSize + 1;
  }

  output[0] = 1;
  return compressed_data_size + 1;
}

int32_t tsDecompressStringImp(const char *const input, int32_t compressedSize, char *const output, int32_t outputSize) {
  // compressedSize is the size of data after compression.

  if (input[0] == 1) {
    /* It is compressed by LZ4 algorithm */
    const int32_t decompressed_size = LZ4_decompress_safe(input + 1, output, compressedSize - 1, outputSize);
    if (decompressed_size < 0) {
      uError("Failed to decompress string with LZ4 algorithm, decompressed size:%d", decompressed_size);
      return -1;
    }

    return decompressed_size;
  } else if (input[0] == 0) {
    /* It is not compressed by LZ4 algorithm */
    memcpy(output, input + 1, compressedSize - 1);
    return compressedSize - 1;
  } else {
    uError("Invalid decompress string indicator:%d", input[0]);
    return -1;
  }
}

/* --------------------------------------------Timestamp Compression
 * ---------------------------------------------- */
// TODO: Take care here, we assumes little endian encoding.
int32_t tsCompressTimestampImp(const char *const input, const int32_t nelements, char *const output) {
  int32_t _pos = 1;
  assert(nelements >= 0);

  if (nelements == 0) return 0;

  int64_t *istream = (int64_t *)input;

  int64_t prev_value = istream[0];
  if (prev_value >= 0x8000000000000000) {
    uWarn("compression timestamp is over signed long long range. ts = 0x%" PRIx64 " \n", prev_value);
    goto _exit_over;
  }
  int64_t  prev_delta = -prev_value;
  uint8_t  flags = 0, flag1 = 0, flag2 = 0;
  uint64_t dd1 = 0, dd2 = 0;

  for (int32_t i = 0; i < nelements; i++) {
    int64_t curr_value = istream[i];
    if (!safeInt64Add(curr_value, -prev_value)) goto _exit_over;
    int64_t curr_delta = curr_value - prev_value;
    if (!safeInt64Add(curr_delta, -prev_delta)) goto _exit_over;
    int64_t delta_of_delta = curr_delta - prev_delta;
    // zigzag encode the value.
    uint64_t zigzag_value = ZIGZAG_ENCODE(int64_t, delta_of_delta);
    if (i % 2 == 0) {
      flags = 0;
      dd1 = zigzag_value;
      if (dd1 == 0) {
        flag1 = 0;
      } else {
        flag1 = (uint8_t)(LONG_BYTES - BUILDIN_CLZL(dd1) / BITS_PER_BYTE);
      }
    } else {
      dd2 = zigzag_value;
      if (dd2 == 0) {
        flag2 = 0;
      } else {
        flag2 = (uint8_t)(LONG_BYTES - BUILDIN_CLZL(dd2) / BITS_PER_BYTE);
      }
      flags = flag1 | (flag2 << 4);
      // Encode the flag.
      if ((_pos + CHAR_BYTES - 1) >= nelements * LONG_BYTES) goto _exit_over;
      memcpy(output + _pos, &flags, CHAR_BYTES);
      _pos += CHAR_BYTES;
      /* Here, we assume it is little endian encoding method. */
      // Encode dd1
      if (is_bigendian()) {
        if ((_pos + flag1 - 1) >= nelements * LONG_BYTES) goto _exit_over;
        memcpy(output + _pos, (char *)(&dd1) + LONG_BYTES - flag1, flag1);
      } else {
        if ((_pos + flag1 - 1) >= nelements * LONG_BYTES) goto _exit_over;
        memcpy(output + _pos, (char *)(&dd1), flag1);
      }
      _pos += flag1;
      // Encode dd2;
      if (is_bigendian()) {
        if ((_pos + flag2 - 1) >= nelements * LONG_BYTES) goto _exit_over;
        memcpy(output + _pos, (char *)(&dd2) + LONG_BYTES - flag2, flag2);
      } else {
        if ((_pos + flag2 - 1) >= nelements * LONG_BYTES) goto _exit_over;
        memcpy(output + _pos, (char *)(&dd2), flag2);
      }
      _pos += flag2;
    }
    prev_value = curr_value;
    prev_delta = curr_delta;
  }

  if (nelements % 2 == 1) {
    flag2 = 0;
    flags = flag1 | (flag2 << 4);
    // Encode the flag.
    if ((_pos + CHAR_BYTES - 1) >= nelements * LONG_BYTES) goto _exit_over;
    memcpy(output + _pos, &flags, CHAR_BYTES);
    _pos += CHAR_BYTES;
    // Encode dd1;
    if (is_bigendian()) {
      if ((_pos + flag1 - 1) >= nelements * LONG_BYTES) goto _exit_over;
      memcpy(output + _pos, (char *)(&dd1) + LONG_BYTES - flag1, flag1);
    } else {
      if ((_pos + flag1 - 1) >= nelements * LONG_BYTES) goto _exit_over;
      memcpy(output + _pos, (char *)(&dd1), flag1);
    }
    _pos += flag1;
  }

  output[0] = 1;  // Means the string is compressed
  return _pos;

_exit_over:
  output[0] = 0;  // Means the string is not compressed
  memcpy(output + 1, input, nelements * LONG_BYTES);
  return nelements * LONG_BYTES + 1;
}

int32_t tsDecompressTimestampImp(const char *const input, const int32_t nelements, char *const output) {
  assert(nelements >= 0);
  if (nelements == 0) return 0;

  if (input[0] == 0) {
    memcpy(output, input + 1, nelements * LONG_BYTES);
    return nelements * LONG_BYTES;
  } else if (input[0] == 1) {  // Decompress
    int64_t *ostream = (int64_t *)output;

    int32_t ipos = 1, opos = 0;
    int8_t  nbytes = 0;
    int64_t prev_value = 0;
    int64_t prev_delta = 0;
    int64_t delta_of_delta = 0;

    while (1) {
      uint8_t flags = input[ipos++];
      // Decode dd1
      uint64_t dd1 = 0;
      nbytes = flags & INT8MASK(4);
      if (nbytes == 0) {
        delta_of_delta = 0;
      } else {
        if (is_bigendian()) {
          memcpy(((char *)(&dd1)) + LONG_BYTES - nbytes, input + ipos, nbytes);
        } else {
          memcpy(&dd1, input + ipos, nbytes);
        }
        delta_of_delta = ZIGZAG_DECODE(int64_t, dd1);
      }
      ipos += nbytes;
      if (opos == 0) {
        prev_value = delta_of_delta;
        prev_delta = 0;
        ostream[opos++] = delta_of_delta;
      } else {
        prev_delta = delta_of_delta + prev_delta;
        prev_value = prev_value + prev_delta;
        ostream[opos++] = prev_value;
      }
      if (opos == nelements) return nelements * LONG_BYTES;

      // Decode dd2
      uint64_t dd2 = 0;
      nbytes = (flags >> 4) & INT8MASK(4);
      if (nbytes == 0) {
        delta_of_delta = 0;
      } else {
        if (is_bigendian()) {
          memcpy(((char *)(&dd2)) + LONG_BYTES - nbytes, input + ipos, nbytes);
        } else {
          memcpy(&dd2, input + ipos, nbytes);
        }
        // zigzag_decoding
        delta_of_delta = ZIGZAG_DECODE(int64_t, dd2);
      }
      ipos += nbytes;
      prev_delta = delta_of_delta + prev_delta;
      prev_value = prev_value + prev_delta;
      ostream[opos++] = prev_value;
      if (opos == nelements) return nelements * LONG_BYTES;
    }

  } else {
    assert(0);
    return -1;
  }
}
/* --------------------------------------------Double Compression
 * ---------------------------------------------- */
void encodeDoubleValue(uint64_t diff, uint8_t flag, char *const output, int32_t *const pos) {
  uint8_t nbytes = (flag & INT8MASK(3)) + 1;
  int32_t nshift = (LONG_BYTES * BITS_PER_BYTE - nbytes * BITS_PER_BYTE) * (flag >> 3);
  diff >>= nshift;

  while (nbytes) {
    output[(*pos)++] = (int8_t)(diff & INT64MASK(8));
    diff >>= BITS_PER_BYTE;
    nbytes--;
  }
}

int32_t tsCompressDoubleImp(const char *const input, const int32_t nelements, char *const output) {
  int32_t byte_limit = nelements * DOUBLE_BYTES + 1;
  int32_t opos = 1;

  uint64_t prev_value = 0;
  uint64_t prev_diff = 0;
  uint8_t  prev_flag = 0;

  double *istream = (double *)input;

  // Main loop
  for (int32_t i = 0; i < nelements; i++) {
    union {
      double   real;
      uint64_t bits;
    } curr;

    curr.real = istream[i];

    // Here we assume the next value is the same as previous one.
    uint64_t predicted = prev_value;
    uint64_t diff = curr.bits ^ predicted;

    int32_t leading_zeros = LONG_BYTES * BITS_PER_BYTE;
    int32_t trailing_zeros = leading_zeros;

    if (diff) {
      trailing_zeros = BUILDIN_CTZL(diff);
      leading_zeros = BUILDIN_CLZL(diff);
    }

    uint8_t nbytes = 0;
    uint8_t flag;

    if (trailing_zeros > leading_zeros) {
      nbytes = (uint8_t)(LONG_BYTES - trailing_zeros / BITS_PER_BYTE);

      if (nbytes > 0) nbytes--;
      flag = ((uint8_t)1 << 3) | nbytes;
    } else {
      nbytes = (uint8_t)(LONG_BYTES - leading_zeros / BITS_PER_BYTE);
      if (nbytes > 0) nbytes--;
      flag = nbytes;
    }

    if (i % 2 == 0) {
      prev_diff = diff;
      prev_flag = flag;
    } else {
      int32_t nbyte1 = (prev_flag & INT8MASK(3)) + 1;
      int32_t nbyte2 = (flag & INT8MASK(3)) + 1;
      if (opos + 1 + nbyte1 + nbyte2 <= byte_limit) {
        uint8_t flags = prev_flag | (flag << 4);
        output[opos++] = flags;
        encodeDoubleValue(prev_diff, prev_flag, output, &opos);
        encodeDoubleValue(diff, flag, output, &opos);
      } else {
        output[0] = 1;
        memcpy(output + 1, input, byte_limit - 1);
        return byte_limit;
      }
    }
    prev_value = curr.bits;
  }

  if (nelements % 2) {
    int32_t nbyte1 = (prev_flag & INT8MASK(3)) + 1;
    int32_t nbyte2 = 1;
    if (opos + 1 + nbyte1 + nbyte2 <= byte_limit) {
      uint8_t flags = prev_flag;
      output[opos++] = flags;
      encodeDoubleValue(prev_diff, prev_flag, output, &opos);
      encodeDoubleValue(0ul, 0, output, &opos);
    } else {
      output[0] = 1;
      memcpy(output + 1, input, byte_limit - 1);
      return byte_limit;
    }
  }

  output[0] = 0;
  return opos;
}

uint64_t decodeDoubleValue(const char *const input, int32_t *const ipos, uint8_t flag) {
  uint64_t diff = 0ul;
  int32_t  nbytes = (flag & INT8MASK(3)) + 1;
  for (int32_t i = 0; i < nbytes; i++) {
    diff = diff | ((INT64MASK(8) & input[(*ipos)++]) << BITS_PER_BYTE * i);
  }
  int32_t shift_width = (LONG_BYTES * BITS_PER_BYTE - nbytes * BITS_PER_BYTE) * (flag >> 3);
  diff <<= shift_width;

  return diff;
}

int32_t tsDecompressDoubleImp(const char *const input, const int32_t nelements, char *const output) {
  // output stream
  double *ostream = (double *)output;

  if (input[0] == 1) {
    memcpy(output, input + 1, nelements * DOUBLE_BYTES);
    return nelements * DOUBLE_BYTES;
  }

  uint8_t  flags = 0;
  int32_t  ipos = 1;
  int32_t  opos = 0;
  uint64_t prev_value = 0;

  for (int32_t i = 0; i < nelements; i++) {
    if (i % 2 == 0) {
      flags = input[ipos++];
    }

    uint8_t flag = flags & INT8MASK(4);
    flags >>= 4;

    uint64_t diff = decodeDoubleValue(input, &ipos, flag);
    union {
      uint64_t bits;
      double   real;
    } curr;

    uint64_t predicted = prev_value;
    curr.bits = predicted ^ diff;
    prev_value = curr.bits;

    ostream[opos++] = curr.real;
  }

  return nelements * DOUBLE_BYTES;
}

/* --------------------------------------------Float Compression
 * ---------------------------------------------- */
void encodeFloatValue(uint32_t diff, uint8_t flag, char *const output, int32_t *const pos) {
  uint8_t nbytes = (flag & INT8MASK(3)) + 1;
  int32_t nshift = (FLOAT_BYTES * BITS_PER_BYTE - nbytes * BITS_PER_BYTE) * (flag >> 3);
  diff >>= nshift;

  while (nbytes) {
    output[(*pos)++] = (int8_t)(diff & INT32MASK(8));
    diff >>= BITS_PER_BYTE;
    nbytes--;
  }
}

int32_t tsCompressFloatImp(const char *const input, const int32_t nelements, char *const output) {
  float  *istream = (float *)input;
  int32_t byte_limit = nelements * FLOAT_BYTES + 1;
  int32_t opos = 1;

  uint32_t prev_value = 0;
  uint32_t prev_diff = 0;
  uint8_t  prev_flag = 0;

  // Main loop
  for (int32_t i = 0; i < nelements; i++) {
    union {
      float    real;
      uint32_t bits;
    } curr;

    curr.real = istream[i];

    // Here we assume the next value is the same as previous one.
    uint32_t predicted = prev_value;
    uint32_t diff = curr.bits ^ predicted;

    int32_t clz = FLOAT_BYTES * BITS_PER_BYTE;
    int32_t ctz = clz;

    if (diff) {
      ctz = BUILDIN_CTZ(diff);
      clz = BUILDIN_CLZ(diff);
    }

    uint8_t nbytes = 0;
    uint8_t flag;

    if (ctz > clz) {
      nbytes = (uint8_t)(FLOAT_BYTES - ctz / BITS_PER_BYTE);

      if (nbytes > 0) nbytes--;
      flag = ((uint8_t)1 << 3) | nbytes;
    } else {
      nbytes = (uint8_t)(FLOAT_BYTES - clz / BITS_PER_BYTE);
      if (nbytes > 0) nbytes--;
      flag = nbytes;
    }

    if (i % 2 == 0) {
      prev_diff = diff;
      prev_flag = flag;
    } else {
      int32_t nbyte1 = (prev_flag & INT8MASK(3)) + 1;
      int32_t nbyte2 = (flag & INT8MASK(3)) + 1;
      if (opos + 1 + nbyte1 + nbyte2 <= byte_limit) {
        uint8_t flags = prev_flag | (flag << 4);
        output[opos++] = flags;
        encodeFloatValue(prev_diff, prev_flag, output, &opos);
        encodeFloatValue(diff, flag, output, &opos);
      } else {
        output[0] = 1;
        memcpy(output + 1, input, byte_limit - 1);
        return byte_limit;
      }
    }
    prev_value = curr.bits;
  }

  if (nelements % 2) {
    int32_t nbyte1 = (prev_flag & INT8MASK(3)) + 1;
    int32_t nbyte2 = 1;
    if (opos + 1 + nbyte1 + nbyte2 <= byte_limit) {
      uint8_t flags = prev_flag;
      output[opos++] = flags;
      encodeFloatValue(prev_diff, prev_flag, output, &opos);
      encodeFloatValue(0, 0, output, &opos);
    } else {
      output[0] = 1;
      memcpy(output + 1, input, byte_limit - 1);
      return byte_limit;
    }
  }

  output[0] = 0;
  return opos;
}

uint32_t decodeFloatValue(const char *const input, int32_t *const ipos, uint8_t flag) {
  uint32_t diff = 0ul;
  int32_t  nbytes = (flag & INT8MASK(3)) + 1;
  for (int32_t i = 0; i < nbytes; i++) {
    diff = diff | ((INT32MASK(8) & input[(*ipos)++]) << BITS_PER_BYTE * i);
  }
  int32_t shift_width = (FLOAT_BYTES * BITS_PER_BYTE - nbytes * BITS_PER_BYTE) * (flag >> 3);
  diff <<= shift_width;

  return diff;
}

int32_t tsDecompressFloatImp(const char *const input, const int32_t nelements, char *const output) {
  float *ostream = (float *)output;

  if (input[0] == 1) {
    memcpy(output, input + 1, nelements * FLOAT_BYTES);
    return nelements * FLOAT_BYTES;
  }

  uint8_t  flags = 0;
  int32_t  ipos = 1;
  int32_t  opos = 0;
  uint32_t prev_value = 0;

  for (int32_t i = 0; i < nelements; i++) {
    if (i % 2 == 0) {
      flags = input[ipos++];
    }

    uint8_t flag = flags & INT8MASK(4);
    flags >>= 4;

    uint32_t diff = decodeFloatValue(input, &ipos, flag);
    union {
      uint32_t bits;
      float    real;
    } curr;

    uint32_t predicted = prev_value;
    curr.bits = predicted ^ diff;
    prev_value = curr.bits;

    ostream[opos++] = curr.real;
  }

  return nelements * FLOAT_BYTES;
}

#ifdef TD_TSZ
//
//   ----------  float double lossy  -----------
//
int32_t tsCompressFloatLossyImp(const char *input, const int32_t nelements, char *const output) {
  // compress with sz
  int32_t       compressedSize = tdszCompress(SZ_FLOAT, input, nelements, output + 1);
  unsigned char algo = ALGO_SZ_LOSSY << 1;
  if (compressedSize == 0 || compressedSize >= nelements * sizeof(float)) {
    // compressed error or large than original
    output[0] = MODE_NOCOMPRESS | algo;
    memcpy(output + 1, input, nelements * sizeof(float));
    compressedSize = 1 + nelements * sizeof(float);
  } else {
    // compressed successfully
    output[0] = MODE_COMPRESS | algo;
    compressedSize += 1;
  }

  return compressedSize;
}

int32_t tsDecompressFloatLossyImp(const char *input, int32_t compressedSize, const int32_t nelements,
                                  char *const output) {
  int32_t decompressedSize = 0;
  if (HEAD_MODE(input[0]) == MODE_NOCOMPRESS) {
    // orginal so memcpy directly
    decompressedSize = nelements * sizeof(float);
    memcpy(output, input + 1, decompressedSize);

    return decompressedSize;
  }

  // decompressed with sz
  return tdszDecompress(SZ_FLOAT, input + 1, compressedSize - 1, nelements, output);
}

int32_t tsCompressDoubleLossyImp(const char *input, const int32_t nelements, char *const output) {
  // compress with sz
  int32_t       compressedSize = tdszCompress(SZ_DOUBLE, input, nelements, output + 1);
  unsigned char algo = ALGO_SZ_LOSSY << 1;
  if (compressedSize == 0 || compressedSize >= nelements * sizeof(double)) {
    // compressed error or large than original
    output[0] = MODE_NOCOMPRESS | algo;
    memcpy(output + 1, input, nelements * sizeof(double));
    compressedSize = 1 + nelements * sizeof(double);
  } else {
    // compressed successfully
    output[0] = MODE_COMPRESS | algo;
    compressedSize += 1;
  }

  return compressedSize;
}

int32_t tsDecompressDoubleLossyImp(const char *input, int32_t compressedSize, const int32_t nelements,
                                   char *const output) {
  int32_t decompressedSize = 0;
  if (HEAD_MODE(input[0]) == MODE_NOCOMPRESS) {
    // orginal so memcpy directly
    decompressedSize = nelements * sizeof(double);
    memcpy(output, input + 1, decompressedSize);

    return decompressedSize;
  }

  // decompressed with sz
  return tdszDecompress(SZ_DOUBLE, input + 1, compressedSize - 1, nelements, output);
}
#endif

/*************************************************************************
 *                  STREAM COMPRESSION
 *************************************************************************/
#define I64_SAFE_ADD(a, b) (((a) >= 0 && (b) <= INT64_MAX - (b)) || ((a) < 0 && (b) >= INT64_MIN - (a)))
typedef struct SCompressor SCompressor;

static int32_t tCompBool(SCompressor *pCmprsor, const void *pData, int32_t nData);
static int32_t tCompInt(SCompressor *pCmprsor, const void *pData, int32_t nData);
static int32_t tCompFloat(SCompressor *pCmprsor, const void *pData, int32_t nData);
static int32_t tCompDouble(SCompressor *pCmprsor, const void *pData, int32_t nData);
static int32_t tCompTimestamp(SCompressor *pCmprsor, const void *pData, int32_t nData);
static int32_t tCompBinary(SCompressor *pCmprsor, const void *pData, int32_t nData);
static struct {
  int8_t  type;
  int32_t bytes;
  int8_t  isVarLen;
  int32_t (*cmprFn)(SCompressor *, const void *, int32_t nData);
} DATA_TYPE_INFO[] = {
    {TSDB_DATA_TYPE_NULL, 0, 0, NULL},                 // TSDB_DATA_TYPE_NULL
    {TSDB_DATA_TYPE_BOOL, 1, 0, tCompBool},            // TSDB_DATA_TYPE_BOOL
    {TSDB_DATA_TYPE_TINYINT, 1, 0, tCompInt},          // TSDB_DATA_TYPE_TINYINT
    {TSDB_DATA_TYPE_SMALLINT, 2, 0, tCompInt},         // TSDB_DATA_TYPE_SMALLINT
    {TSDB_DATA_TYPE_INT, 4, 0, tCompInt},              // TSDB_DATA_TYPE_INT
    {TSDB_DATA_TYPE_BIGINT, 8, 0, tCompInt},           // TSDB_DATA_TYPE_BIGINT
    {TSDB_DATA_TYPE_FLOAT, 4, 0, tCompFloat},          // TSDB_DATA_TYPE_FLOAT
    {TSDB_DATA_TYPE_DOUBLE, 8, 0, tCompDouble},        // TSDB_DATA_TYPE_DOUBLE
    {TSDB_DATA_TYPE_VARCHAR, 1, 1, tCompBinary},       // TSDB_DATA_TYPE_VARCHAR
    {TSDB_DATA_TYPE_TIMESTAMP, 8, 0, tCompTimestamp},  // pTSDB_DATA_TYPE_TIMESTAMP
    {TSDB_DATA_TYPE_NCHAR, 1, 1, tCompBinary},         // TSDB_DATA_TYPE_NCHAR
    {TSDB_DATA_TYPE_UTINYINT, 1, 0, tCompInt},         // TSDB_DATA_TYPE_UTINYINT
    {TSDB_DATA_TYPE_USMALLINT, 2, 0, tCompInt},        // TSDB_DATA_TYPE_USMALLINT
    {TSDB_DATA_TYPE_UINT, 4, 0, tCompInt},             // TSDB_DATA_TYPE_UINT
    {TSDB_DATA_TYPE_UBIGINT, 8, 0, tCompInt},          // TSDB_DATA_TYPE_UBIGINT
    {TSDB_DATA_TYPE_JSON, 1, 1, tCompBinary},          // TSDB_DATA_TYPE_JSON
    {TSDB_DATA_TYPE_VARBINARY, 1, 1, tCompBinary},     // TSDB_DATA_TYPE_VARBINARY
    {TSDB_DATA_TYPE_DECIMAL, 1, 1, tCompBinary},       // TSDB_DATA_TYPE_DECIMAL
    {TSDB_DATA_TYPE_BLOB, 1, 1, tCompBinary},          // TSDB_DATA_TYPE_BLOB
    {TSDB_DATA_TYPE_MEDIUMBLOB, 1, 1, tCompBinary},    // TSDB_DATA_TYPE_MEDIUMBLOB
};

struct SCompressor {
  int8_t   type;
  int8_t   cmprAlg;
  int8_t   autoAlloc;
  int32_t  nVal;
  uint8_t *aBuf[2];
  int64_t  nBuf[2];
  union {
    // Timestamp ----
    struct {
      int64_t  ts_prev_val;
      int64_t  ts_prev_delta;
      uint8_t *ts_flag_p;
    };
    // Integer ----
    struct {
      int64_t  i_prev;
      int32_t  i_selector;
      int32_t  i_start;
      int32_t  i_end;
      uint64_t i_aZigzag[241];
      int8_t   i_aBitN[241];
    };
    // Float ----
    struct {
      uint32_t f_prev;
      uint8_t *f_flag_p;
    };
    // Double ----
    struct {
      uint64_t d_prev;
      uint8_t *d_flag_p;
    };
  };
};

// Timestamp =====================================================
static int32_t tCompSetCopyMode(SCompressor *pCmprsor) {
  int32_t code = 0;

  if (pCmprsor->nVal) {
    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[1], sizeof(int64_t) * pCmprsor->nVal);
      if (code) return code;
    }
    pCmprsor->nBuf[1] = 0;

    int64_t  n = 1;
    int64_t  value;
    int64_t  delta;
    uint64_t vZigzag;
    while (n < pCmprsor->nBuf[0]) {
      uint8_t aN[2];
      aN[0] = pCmprsor->aBuf[0][n] & 0xf;
      aN[1] = pCmprsor->aBuf[0][n] >> 4;

      n++;

      for (int32_t i = 0; i < 2; i++) {
        vZigzag = 0;
        for (uint8_t j = 0; j < aN[i]; j++) {
          vZigzag |= (((uint64_t)pCmprsor->aBuf[0][n]) << (8 * j));
          n++;
        }

        int64_t delta_of_delta = ZIGZAG_DECODE(int64_t, vZigzag);
        if (pCmprsor->nBuf[1] == 0) {
          delta = 0;
          value = delta_of_delta;
        } else {
          delta = delta_of_delta + delta;
          value = delta + value;
        }

        memcpy(pCmprsor->aBuf[1] + pCmprsor->nBuf[1], &value, sizeof(int64_t));
        pCmprsor->nBuf[1] += sizeof(int64_t);

        if (n >= pCmprsor->nBuf[0]) break;
      }
    }

    ASSERT(n == pCmprsor->nBuf[0]);

    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[1] + 1);
      if (code) return code;
    }
    memcpy(pCmprsor->aBuf[0] + 1, pCmprsor->aBuf[1], pCmprsor->nBuf[1]);
    pCmprsor->nBuf[0] = 1 + pCmprsor->nBuf[1];
  }
  pCmprsor->aBuf[0][0] = 0;

  return code;
}
static int32_t tCompTimestamp(SCompressor *pCmprsor, const void *pData, int32_t nData) {
  int32_t code = 0;

  int64_t ts = *(int64_t *)pData;
  ASSERT(pCmprsor->type == TSDB_DATA_TYPE_TIMESTAMP);
  ASSERT(nData == 8);

  if (pCmprsor->aBuf[0][0] == 1) {
    if (pCmprsor->nVal == 0) {
      pCmprsor->ts_prev_val = ts;
      pCmprsor->ts_prev_delta = -ts;
    }

    if (!I64_SAFE_ADD(ts, -pCmprsor->ts_prev_val)) {
      code = tCompSetCopyMode(pCmprsor);
      if (code) return code;
      goto _copy_cmpr;
    }
    int64_t delta = ts - pCmprsor->ts_prev_val;

    if (!I64_SAFE_ADD(delta, -pCmprsor->ts_prev_delta)) {
      code = tCompSetCopyMode(pCmprsor);
      if (code) return code;
      goto _copy_cmpr;
    }
    int64_t  delta_of_delta = delta - pCmprsor->ts_prev_delta;
    uint64_t vZigzag = ZIGZAG_ENCODE(int64_t, delta_of_delta);

    pCmprsor->ts_prev_val = ts;
    pCmprsor->ts_prev_delta = delta;

    if ((pCmprsor->nVal & 0x1) == 0) {
      if (pCmprsor->autoAlloc) {
        code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + 17);
        if (code) return code;
      }

      pCmprsor->ts_flag_p = pCmprsor->aBuf[0] + pCmprsor->nBuf[0];
      pCmprsor->nBuf[0]++;
      pCmprsor->ts_flag_p[0] = 0;
      while (vZigzag) {
        pCmprsor->aBuf[0][pCmprsor->nBuf[0]] = (vZigzag & 0xff);
        pCmprsor->nBuf[0]++;
        pCmprsor->ts_flag_p[0]++;
        vZigzag >>= 8;
      }
    } else {
      while (vZigzag) {
        pCmprsor->aBuf[0][pCmprsor->nBuf[0]] = (vZigzag & 0xff);
        pCmprsor->nBuf[0]++;
        pCmprsor->ts_flag_p[0] += 0x10;
        vZigzag >>= 8;
      }
    }
  } else {
  _copy_cmpr:
    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + sizeof(ts));
      if (code) return code;
    }

    memcpy(pCmprsor->aBuf[0] + pCmprsor->nBuf[0], &ts, sizeof(ts));
    pCmprsor->nBuf[0] += sizeof(ts);
  }
  pCmprsor->nVal++;

  return code;
}

// Integer =====================================================
#define SIMPLE8B_MAX ((uint64_t)1152921504606846974LL)
static const uint8_t BIT_PER_INTEGER[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60};
static const int32_t SELECTOR_TO_ELEMS[] = {240, 120, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1};
static const uint8_t BIT_TO_SELECTOR[] = {0,  2,  3,  4,  5,  6,  7,  8,  9,  10, 10, 11, 11, 12, 12, 12,
                                          13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15,
                                          15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                          15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15};

static int32_t tCompInt(SCompressor *pCmprsor, const void *pData, int32_t nData) {
  int32_t code = 0;

  ASSERT(nData == DATA_TYPE_INFO[pCmprsor->type].bytes);

  if (pCmprsor->aBuf[0][0] == 0) {
    int64_t val;

    switch (pCmprsor->type) {
      case TSDB_DATA_TYPE_TINYINT:
        val = *(int8_t *)pData;
        break;
      case TSDB_DATA_TYPE_SMALLINT:
        val = *(int16_t *)pData;
        break;
      case TSDB_DATA_TYPE_INT:
        val = *(int32_t *)pData;
        break;
      case TSDB_DATA_TYPE_BIGINT:
        val = *(int64_t *)pData;
        break;
      case TSDB_DATA_TYPE_UTINYINT:
        val = *(uint8_t *)pData;
        break;
      case TSDB_DATA_TYPE_USMALLINT:
        val = *(uint16_t *)pData;
        break;
      case TSDB_DATA_TYPE_UINT:
        val = *(uint32_t *)pData;
        break;
      case TSDB_DATA_TYPE_UBIGINT:
        val = *(int64_t *)pData;
        break;
      default:
        ASSERT(0);
        break;
    }

    if (!I64_SAFE_ADD(val, -pCmprsor->i_prev)) {
      // TODO
      goto _copy_cmpr;
    }

    int64_t  diff = val - pCmprsor->i_prev;
    uint64_t vZigzag = ZIGZAG_ENCODE(int64_t, diff);
    if (vZigzag >= SIMPLE8B_MAX) {
      // TODO
      goto _copy_cmpr;
    }

    int8_t nBit = (vZigzag) ? (64 - BUILDIN_CLZL(vZigzag)) : 0;
    pCmprsor->i_prev = val;

    while (1) {
      int32_t nEle = (pCmprsor->i_end + 241 - pCmprsor->i_start) % 241;

      if (nEle + 1 <= SELECTOR_TO_ELEMS[pCmprsor->i_selector] && nEle + 1 <= SELECTOR_TO_ELEMS[BIT_TO_SELECTOR[nBit]]) {
        if (pCmprsor->i_selector < BIT_TO_SELECTOR[nBit]) {
          pCmprsor->i_selector = BIT_TO_SELECTOR[nBit];
        }
        pCmprsor->i_end = (pCmprsor->i_end + 1) % 241;
        pCmprsor->i_aZigzag[pCmprsor->i_end] = vZigzag;
        pCmprsor->i_aBitN[pCmprsor->i_end] = nBit;
        break;
      } else {
        while (nEle < SELECTOR_TO_ELEMS[pCmprsor->i_selector]) {
          pCmprsor->i_selector++;
        }
        nEle = SELECTOR_TO_ELEMS[pCmprsor->i_selector];

        if (pCmprsor->autoAlloc) {
          code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + sizeof(uint64_t));
          if (code) return code;
        }

        uint64_t *bp = (uint64_t *)(pCmprsor->aBuf[0] + pCmprsor->nBuf[0]);
        pCmprsor->nBuf[0] += sizeof(uint64_t);
        bp[0] = pCmprsor->i_selector;
        uint8_t bits = BIT_PER_INTEGER[pCmprsor->i_selector];
        for (int32_t iVal = 0; iVal < nEle; iVal++) {
          bp[0] |= ((pCmprsor->i_aZigzag[pCmprsor->i_start] & ((((uint64_t)1) << bits) - 1)) << (bits * iVal + 4));
          pCmprsor->i_start = (pCmprsor->i_start + 1) % 241;
        }

        // reset and continue
        pCmprsor->i_selector = 0;
        for (int32_t iVal = pCmprsor->i_start; iVal < pCmprsor->i_end; iVal = (iVal + 1) % 241) {
          if (pCmprsor->i_selector < BIT_TO_SELECTOR[pCmprsor->i_aBitN[iVal]]) {
            pCmprsor->i_selector = BIT_TO_SELECTOR[pCmprsor->i_aBitN[iVal]];
          }
        }
      }
    }
  } else {
  _copy_cmpr:
    code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + nData);
    if (code) return code;

    memcpy(pCmprsor->aBuf[0] + pCmprsor->nBuf[0], pData, nData);
    pCmprsor->nBuf[0] += nData;
  }
  pCmprsor->nVal++;

  return code;
}

// Float =====================================================
static int32_t tCompFloat(SCompressor *pCmprsor, const void *pData, int32_t nData) {
  int32_t code = 0;

  ASSERT(nData == sizeof(float));

  union {
    float    f;
    uint32_t u;
  } val = {.f = *(float *)pData};

  uint32_t diff = val.u ^ pCmprsor->f_prev;
  pCmprsor->f_prev = val.u;

  int32_t clz, ctz;
  if (diff) {
    clz = BUILDIN_CLZ(diff);
    ctz = BUILDIN_CTZ(diff);
  } else {
    clz = 32;
    ctz = 32;
  }

  uint8_t nBytes;
  if (clz < ctz) {
    nBytes = sizeof(uint32_t) - ctz / BITS_PER_BYTE;
    if (nBytes) diff >>= (32 - nBytes * BITS_PER_BYTE);
  } else {
    nBytes = sizeof(uint32_t) - clz / BITS_PER_BYTE;
  }
  if (nBytes == 0) nBytes++;

  if ((pCmprsor->nVal & 0x1) == 0) {
    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + 9);
      if (code) return code;
    }

    pCmprsor->f_flag_p = &pCmprsor->aBuf[0][pCmprsor->nBuf[0]];
    pCmprsor->nBuf[0]++;

    if (clz < ctz) {
      pCmprsor->f_flag_p[0] = (0x08 | (nBytes - 1));
    } else {
      pCmprsor->f_flag_p[0] = nBytes - 1;
    }
  } else {
    if (clz < ctz) {
      pCmprsor->f_flag_p[0] |= ((0x08 | (nBytes - 1)) << 4);
    } else {
      pCmprsor->f_flag_p[0] |= ((nBytes - 1) << 4);
    }
  }
  for (; nBytes; nBytes--) {
    pCmprsor->aBuf[0][pCmprsor->nBuf[0]] = (diff & 0xff);
    pCmprsor->nBuf[0]++;
    diff >>= BITS_PER_BYTE;
  }
  pCmprsor->nVal++;

  return code;
}

// Double =====================================================
static int32_t tCompDouble(SCompressor *pCmprsor, const void *pData, int32_t nData) {
  int32_t code = 0;

  ASSERT(nData == sizeof(double));

  union {
    double   d;
    uint64_t u;
  } val = {.d = *(double *)pData};

  uint64_t diff = val.u ^ pCmprsor->d_prev;
  pCmprsor->d_prev = val.u;

  int32_t clz, ctz;
  if (diff) {
    clz = BUILDIN_CLZL(diff);
    ctz = BUILDIN_CTZL(diff);
  } else {
    clz = 64;
    ctz = 64;
  }

  uint8_t nBytes;
  if (clz < ctz) {
    nBytes = sizeof(uint64_t) - ctz / BITS_PER_BYTE;
    if (nBytes) diff >>= (64 - nBytes * BITS_PER_BYTE);
  } else {
    nBytes = sizeof(uint64_t) - clz / BITS_PER_BYTE;
  }
  if (nBytes == 0) nBytes++;

  if ((pCmprsor->nVal & 0x1) == 0) {
    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + 17);
      if (code) return code;
    }

    pCmprsor->d_flag_p = &pCmprsor->aBuf[0][pCmprsor->nBuf[0]];
    pCmprsor->nBuf[0]++;

    if (clz < ctz) {
      pCmprsor->d_flag_p[0] = (0x08 | (nBytes - 1));
    } else {
      pCmprsor->d_flag_p[0] = nBytes - 1;
    }
  } else {
    if (clz < ctz) {
      pCmprsor->d_flag_p[0] |= ((0x08 | (nBytes - 1)) << 4);
    } else {
      pCmprsor->d_flag_p[0] |= ((nBytes - 1) << 4);
    }
  }
  for (; nBytes; nBytes--) {
    pCmprsor->aBuf[0][pCmprsor->nBuf[0]] = (diff & 0xff);
    pCmprsor->nBuf[0]++;
    diff >>= BITS_PER_BYTE;
  }
  pCmprsor->nVal++;

  return code;
}

// Binary =====================================================
static int32_t tCompBinary(SCompressor *pCmprsor, const void *pData, int32_t nData) {
  int32_t code = 0;

  if (nData) {
    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0] + nData);
      if (code) return code;
    }

    memcpy(pCmprsor->aBuf[0] + pCmprsor->nBuf[0], pData, nData);
    pCmprsor->nBuf[0] += nData;
  }
  pCmprsor->nVal++;

  return code;
}

// Bool =====================================================
static const uint8_t BOOL_CMPR_TABLE[] = {0b01, 0b0100, 0b010000, 0b01000000};

static int32_t tCompBool(SCompressor *pCmprsor, const void *pData, int32_t nData) {
  int32_t code = 0;

  bool vBool = *(int8_t *)pData;

  int32_t mod4 = pCmprsor->nVal & 3;
  if (mod4 == 0) {
    pCmprsor->nBuf[0]++;

    if (pCmprsor->autoAlloc) {
      code = tRealloc(&pCmprsor->aBuf[0], pCmprsor->nBuf[0]);
      if (code) return code;
    }

    pCmprsor->aBuf[0][pCmprsor->nBuf[0] - 1] = 0;
  }
  if (vBool) {
    pCmprsor->aBuf[0][pCmprsor->nBuf[0] - 1] |= BOOL_CMPR_TABLE[mod4];
  }
  pCmprsor->nVal++;

  return code;
}

// SCompressor =====================================================
int32_t tCompressorCreate(SCompressor **ppCmprsor) {
  int32_t code = 0;

  *ppCmprsor = (SCompressor *)taosMemoryCalloc(1, sizeof(SCompressor));
  if ((*ppCmprsor) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  code = tRealloc(&(*ppCmprsor)->aBuf[0], 1024);
  if (code) {
    taosMemoryFree(*ppCmprsor);
    *ppCmprsor = NULL;
    goto _exit;
  }

_exit:
  return code;
}

int32_t tCompressorDestroy(SCompressor *pCmprsor) {
  int32_t code = 0;

  if (pCmprsor) {
    int32_t nBuf = sizeof(pCmprsor->aBuf) / sizeof(pCmprsor->aBuf[0]);
    for (int32_t iBuf = 0; iBuf < nBuf; iBuf++) {
      tFree(pCmprsor->aBuf[iBuf]);
    }

    taosMemoryFree(pCmprsor);
  }

  return code;
}

int32_t tCompressorReset(SCompressor *pCmprsor, int8_t type, int8_t cmprAlg, int8_t autoAlloc) {
  int32_t code = 0;

  pCmprsor->type = type;
  pCmprsor->cmprAlg = cmprAlg;
  pCmprsor->autoAlloc = autoAlloc;
  pCmprsor->nVal = 0;

  switch (type) {
    case TSDB_DATA_TYPE_TIMESTAMP:
      pCmprsor->ts_prev_val = 0;
      pCmprsor->ts_prev_delta = 0;
      pCmprsor->ts_flag_p = NULL;
      pCmprsor->aBuf[0][0] = 1;  // For timestamp, 1 means compressed, 0 otherwise
      pCmprsor->nBuf[0] = 1;
      break;
    case TSDB_DATA_TYPE_BOOL:
      pCmprsor->nBuf[0] = 0;
      break;
    case TSDB_DATA_TYPE_BINARY:
      pCmprsor->nBuf[0] = 0;
      break;
    case TSDB_DATA_TYPE_FLOAT:
      pCmprsor->f_prev = 0;
      pCmprsor->f_flag_p = NULL;
      pCmprsor->aBuf[0][0] = 0;  // 0 means compressed, 1 otherwise (for backward compatibility)
      pCmprsor->nBuf[0] = 1;
      break;
    case TSDB_DATA_TYPE_DOUBLE:
      pCmprsor->d_prev = 0;
      pCmprsor->d_flag_p = NULL;
      pCmprsor->aBuf[0][0] = 0;  // 0 means compressed, 1 otherwise (for backward compatibility)
      pCmprsor->nBuf[0] = 1;
      break;
    case TSDB_DATA_TYPE_TINYINT:
    case TSDB_DATA_TYPE_SMALLINT:
    case TSDB_DATA_TYPE_INT:
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_UTINYINT:
    case TSDB_DATA_TYPE_USMALLINT:
    case TSDB_DATA_TYPE_UINT:
    case TSDB_DATA_TYPE_UBIGINT:
      pCmprsor->i_prev = 0;
      pCmprsor->i_selector = 0;
      pCmprsor->i_start = 0;
      pCmprsor->i_end = 0;
      pCmprsor->aBuf[0][0] = 0;  // 0 means compressed, 1 otherwise (for backward compatibility)
      pCmprsor->nBuf[0] = 1;
      break;
    default:
      break;
  }

  return code;
}

int32_t tCompGen(SCompressor *pCmprsor, const uint8_t **ppData, int64_t *nData) {
  int32_t code = 0;

  if (pCmprsor->nVal == 0) {
    *ppData = NULL;
    *nData = 0;
    return code;
  }

  if (pCmprsor->cmprAlg == TWO_STAGE_COMP /*|| IS_VAR_DATA_TYPE(pCmprsor->type)*/) {
    code = tRealloc(&pCmprsor->aBuf[1], pCmprsor->nBuf[0] + 1);
    if (code) return code;

    int64_t ret = LZ4_compress_default(pCmprsor->aBuf[0], pCmprsor->aBuf[1] + 1, pCmprsor->nBuf[0], pCmprsor->nBuf[0]);
    if (ret) {
      pCmprsor->aBuf[1][0] = 0;
      pCmprsor->nBuf[1] = ret + 1;
    } else {
      pCmprsor->aBuf[1][0] = 1;
      memcpy(pCmprsor->aBuf[1] + 1, pCmprsor->aBuf[0], pCmprsor->nBuf[0]);
      pCmprsor->nBuf[1] = pCmprsor->nBuf[0] + 1;
    }

    *ppData = pCmprsor->aBuf[1];
    *nData = pCmprsor->nBuf[1];
  } else {
    *ppData = pCmprsor->aBuf[0];
    *nData = pCmprsor->nBuf[0];
  }

  return code;
}

int32_t tCompress(SCompressor *pCmprsor, const void *pData, int64_t nData) {
  return DATA_TYPE_INFO[pCmprsor->type].cmprFn(pCmprsor, pData, nData);
}