/*
 * Copyright © 2013-2017  Rinat Ibragimov
 *
 * This file is part of FreshPlayerPlugin.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "encoding_alias.h"
#include "pp_interface.h"
#include "ppb_char_set.h"
#include "ppb_memory.h"
#include "ppb_var.h"
#include "reverse_constant.h"
#include "trace_core.h"
#include <errno.h>
#include <iconv.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *
ppb_char_set_utf16_to_char_set(PP_Instance instance, const uint16_t *utf16, uint32_t utf16_len,
                               const char *output_char_set,
                               enum PP_CharSet_ConversionError on_error, uint32_t *output_length)
{
    // each character could take up to 4 bytes in UTF-8; with additional zero-terminator byte
    const uint32_t output_buffer_length = (utf16_len + 1) * 4 + 1;
    char *output = ppb_memory_mem_alloc(output_buffer_length);

    if (!output) {
        trace_error("%s, can't allocate memory, %u bytes\n", __func__, output_buffer_length);
        goto err;
    }

    const char *charset = encoding_alias_get_canonical_name(output_char_set);

    char to_code[128];
    if (on_error == PP_CHARSET_CONVERSIONERROR_SKIP) {
        snprintf(to_code, sizeof(to_code), "%s//IGNORE", charset);
    } else {
        snprintf(to_code, sizeof(to_code), "%s", charset);
    }

    iconv_t cd = iconv_open(to_code, "UTF-16LE");
    if (cd == (iconv_t)-1) {
        trace_error("%s, wrong charset %s\n", __func__, output_char_set);
        goto err;
    }

    char *inbuf = (char *)utf16;
    size_t inbytesleft = utf16_len * sizeof(uint16_t);
    char *outbuf = output;
    // reserve one byte for trailing nul
    size_t outbytesleft = output_buffer_length - 1;

    while (inbytesleft > 0) {
        if (iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft) != (size_t)-1) {
            break;
        }

        if (errno == E2BIG) {
            break;
        }

        if (errno == EILSEQ || errno == EINVAL) {
            if (on_error == PP_CHARSET_CONVERSIONERROR_SUBSTITUTE) {
                if (outbytesleft < 1)
                    break;
                *outbuf++ = '?';
                outbytesleft--;

                size_t step = (inbytesleft >= sizeof(uint16_t)) ? sizeof(uint16_t) : inbytesleft;
                inbuf += step;
                inbytesleft -= step;
                // reset shift state after skipping invalid sequence
                iconv(cd, NULL, NULL, &outbuf, &outbytesleft);
                continue;
            } else if (on_error == PP_CHARSET_CONVERSIONERROR_SKIP) {
                size_t step = (inbytesleft >= sizeof(uint16_t)) ? sizeof(uint16_t) : inbytesleft;
                inbuf += step;
                inbytesleft -= step;
                continue;
            }
        }

        goto err;
    }

    // flush any remaining shift sequence
    iconv(cd, NULL, NULL, &outbuf, &outbytesleft);

    *outbuf = '\0';
    *output_length = (uint32_t)(outbuf - output);

    iconv_close(cd);
    return output;

err:
    *output_length = 0;
    ppb_memory_mem_free(output);
    if (cd != (iconv_t)-1)
        iconv_close(cd);
    return NULL;
}

uint16_t *
ppb_char_set_char_set_to_utf16(PP_Instance instance, const char *input, uint32_t input_len,
                               const char *input_char_set, enum PP_CharSet_ConversionError on_error,
                               uint32_t *output_length)
{
    // each character could be converted into a surrogate pair
    const uint32_t output_buffer_length = (input_len + 2) * 2 * sizeof(uint16_t);
    uint16_t *output = ppb_memory_mem_alloc(output_buffer_length);

    if (!output) {
        trace_error("%s, can't allocate memory, %u bytes\n", __func__, output_buffer_length);
        goto err;
    }

    const char *charset = encoding_alias_get_canonical_name(input_char_set);

    const char *to_code = (on_error == PP_CHARSET_CONVERSIONERROR_SKIP)
                          ? "UTF-16LE//IGNORE"
                          : "UTF-16LE";

    iconv_t cd = iconv_open(to_code, charset);
    if (cd == (iconv_t)-1) {
        trace_error("%s, wrong charset %s\n", __func__, input_char_set);
        goto err;
    }

    char *inbuf = (char *)input;
    size_t inbytesleft = input_len;
    char *outbuf = (char *)output;
    // reserve two bytes for trailing utf-16 nul
    size_t outbytesleft = output_buffer_length - sizeof(uint16_t);

    while (inbytesleft > 0) {
        if (iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft) != (size_t)-1) {
            break;
        }

        if (errno == E2BIG) {
            break;
        }

        if (errno == EILSEQ || errno == EINVAL) {
            if (on_error == PP_CHARSET_CONVERSIONERROR_SUBSTITUTE) {
                if (outbytesleft < sizeof(uint16_t))
                    break;
                *(uint16_t *)outbuf = (uint16_t)'?';
                outbuf += sizeof(uint16_t);
                outbytesleft -= sizeof(uint16_t);

                inbuf += 1;
                inbytesleft -= 1;
                iconv(cd, NULL, NULL, NULL, NULL);
                continue;
            } else if (on_error == PP_CHARSET_CONVERSIONERROR_SKIP) {
                inbuf += 1;
                inbytesleft -= 1;
                continue;
            }
        }

        goto err;
    }

    *(uint16_t *)outbuf = 0;
    *output_length = (uint32_t)((outbuf - (char *)output) / sizeof(uint16_t));

    iconv_close(cd);
    return output;

err:
    *output_length = 0;
    ppb_memory_mem_free(output);
    if (cd != (iconv_t)-1)
        iconv_close(cd);
    return NULL;
}

static
char *
extract_relevant_part_from_locale_name(const char *locale_name)
{
    char *lang = strdup(locale_name ? locale_name : "en");

    if (strncmp(lang, "zh", 2) == 0) {
        // starts with zh. For Chinese languages country is required too

        // replace "_" by "-". That way "zh_CN.utf8" becomes "zh-CN.utf8"
        char *ptr = strchr(lang, '_');
        if (ptr)
            *ptr = '-';

        // cut at ".". That way "zh-CN.utf8" becomes "zh-CN"
        ptr = strchr(lang, '.');
        if (ptr)
            *ptr = '\0';

        return lang;
    }

    // otherwise, leave only language. Cut at "_". That makes "ru" from "ru_RU.utf8"
    char *ptr = strchr(lang,  '_');
    if (ptr)
        *ptr = '\0';

    return lang;
}

struct PP_Var
ppb_char_set_get_default_char_set(PP_Instance instance)
{
    char *lang = extract_relevant_part_from_locale_name(getenv("LANG"));

    struct lang_encoding {
        const char *lang;
        const char *encoding;
    };

    static const struct lang_encoding pairs[] = {
        {"am", "windows-1252"},
        {"ar", "windows-1256"},
        {"bg", "windows-1251"},
        {"bn", "windows-1252"},
        {"ca", "windows-1252"},
        {"cs", "windows-1250"},
        {"da", "windows-1252"},
        {"de", "windows-1252"},
        {"el", "ISO-8859-7"},
        {"en", "windows-1252"},
        {"es", "windows-1252"},
        {"et", "windows-1257"},
        {"fa", "windows-1256"},
        {"fil", "windows-1252"},
        {"fi", "windows-1252"},
        {"fr", "windows-1252"},
        {"gu", "windows-1252"},
        {"he", "windows-1255"},
        {"hi", "windows-1252"},
        {"hr", "windows-1250"},
        {"hu", "ISO-8859-2"},
        {"id", "windows-1252"},
        {"it", "windows-1252"},
        {"ja", "Shift_JIS"},
        {"kn", "windows-1252"},
        {"ko", "UHC"},
        {"lt", "windows-1257"},
        {"lv", "windows-1257"},
        {"ml", "windows-1252"},
        {"mr", "windows-1252"},
        {"nb", "windows-1252"},
        {"nl", "windows-1252"},
        {"pl", "ISO-8859-2"},
        {"pt", "windows-1252"},
        {"pt", "windows-1252"},
        {"ro", "ISO-8859-2"},
        {"ru", "windows-1251"},
        {"sk", "windows-1250"},
        {"sl", "ISO-8859-2"},
        {"sr", "windows-1251"},
        {"sv", "windows-1252"},
        {"sw", "windows-1252"},
        {"ta", "windows-1252"},
        {"te", "windows-1252"},
        {"th", "windows-874"},
        {"tr", "ISO-8859-9"},
        {"uk", "windows-1251"},
        {"vi", "windows-1258"},
        {"zh-CN", "GBK"},
        {"zh-TW", "Big5"},
    };

    const char *enc = "windows-1252";
    for (uint32_t k = 0; k < sizeof(pairs)/sizeof(pairs[0]); k ++) {
        if (strcasecmp(pairs[k].lang, lang) == 0) {
            enc = pairs[k].encoding;
            break;
        }
    }

    struct PP_Var ret = ppb_var_var_from_utf8_z(enc);
    free(lang);
    return ret;
}


// trace wrappers
TRACE_WRAPPER
char *
trace_ppb_char_set_utf16_to_char_set(PP_Instance instance, const uint16_t *utf16,
                                     uint32_t utf16_len, const char *output_char_set,
                                     enum PP_CharSet_ConversionError on_error,
                                     uint32_t *output_length)
{
    trace_info("[PPB] {full} %s instance=%d, utf16=%p, utf16_len=%u, output_char_set=%s, "
               "on_error=%s\n", __func__+6, instance, utf16, utf16_len, output_char_set,
               reverse_char_set_conversion_error(on_error));
    return ppb_char_set_utf16_to_char_set(instance, utf16, utf16_len, output_char_set, on_error,
                                          output_length);
}

TRACE_WRAPPER
uint16_t *
trace_ppb_char_set_char_set_to_utf16(PP_Instance instance, const char *input, uint32_t input_len,
                                     const char *input_char_set,
                                     enum PP_CharSet_ConversionError on_error,
                                     uint32_t *output_length)
{
    trace_info("[PPB] {full} %s instance=%d, input=%.*s, input_len=%u, input_char_set=%s, "
               "on_error=%s\n", __func__+6, instance, input_len, input, input_len, input_char_set,
               reverse_char_set_conversion_error(on_error));
    return ppb_char_set_char_set_to_utf16(instance, input, input_len, input_char_set, on_error,
                                          output_length);
}

TRACE_WRAPPER
struct PP_Var
trace_ppb_char_set_get_default_char_set(PP_Instance instance)
{
    trace_info("[PPB] {full} %s instance=%d\n", __func__+6, instance);
    return ppb_char_set_get_default_char_set(instance);
}


const struct PPB_CharSet_Dev_0_4 ppb_char_set_dev_interface_0_4 = {
    .UTF16ToCharSet =       TWRAPF(ppb_char_set_utf16_to_char_set),
    .CharSetToUTF16 =       TWRAPF(ppb_char_set_char_set_to_utf16),
    .GetDefaultCharSet =    TWRAPF(ppb_char_set_get_default_char_set),
};

static
void
__attribute__((constructor))
constructor_ppb_char_set(void)
{
    register_interface(PPB_CHAR_SET_DEV_INTERFACE_0_4, &ppb_char_set_dev_interface_0_4);
}
