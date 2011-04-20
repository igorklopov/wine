/*
 * Copyright 2011 Piotr Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef PRINTF_WIDE
#define APICHAR MSVCRT_wchar_t
#define CONVCHAR char
#define FUNC_NAME(func) func ## _w
#else
#define APICHAR char
#define CONVCHAR MSVCRT_wchar_t
#define FUNC_NAME(func) func ## _a
#endif

typedef struct FUNC_NAME(pf_flags_t)
{
    APICHAR Sign, LeftAlign, Alternate, PadZero;
    int FieldLength, Precision;
    APICHAR IntegerLength, IntegerDouble;
    APICHAR WideString;
    APICHAR Format;
} FUNC_NAME(pf_flags);

struct FUNC_NAME(_str_ctx) {
    MSVCRT_size_t len;
    APICHAR *buf;
};

static int FUNC_NAME(puts_clbk_str)(void *ctx, int len, const APICHAR *str)
{
    struct FUNC_NAME(_str_ctx) *out = ctx;

    if(!out->buf)
        return len;

    if(out->len < len) {
        memcpy(out->buf, str, out->len);
        out->buf += out->len;
        out->len = 0;
        return -1;
    }

    memcpy(out->buf, str, len*sizeof(APICHAR));
    out->buf += len;
    return len;
}

static inline const APICHAR* FUNC_NAME(pf_parse_int)(const APICHAR *fmt, int *val)
{
    *val = 0;

    while(isdigit(*fmt)) {
        *val *= 10;
        *val += *fmt++ - '0';
    }

    return fmt;
}

/* pf_fill: takes care of signs, alignment, zero and field padding */
static inline int FUNC_NAME(pf_fill)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx,
        int len, FUNC_NAME(pf_flags) *flags, BOOL left)
{
    int i, r = 0, written;

    if(flags->Sign && !(flags->Format=='d' || flags->Format=='i'))
        flags->Sign = 0;

    if(left && flags->Sign) {
        flags->FieldLength--;
        if(flags->PadZero)
            r = pf_puts(puts_ctx, 1, &flags->Sign);
    }
    written = r;

    if((!left && flags->LeftAlign) || (left && !flags->LeftAlign)) {
        APICHAR ch;

        if(left && flags->PadZero)
            ch = '0';
        else
            ch = ' ';

        for(i=0; i<flags->FieldLength-len && r>=0; i++) {
            r = pf_puts(puts_ctx, 1, &ch);
            written += r;
        }
    }


    if(r>=0 && left && flags->Sign && !flags->PadZero) {
        r = pf_puts(puts_ctx, 1, &flags->Sign);
        written += r;
    }

    return r>=0 ? written : r;
}

static inline int FUNC_NAME(pf_output_wstr)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx,
        const MSVCRT_wchar_t *str, int len, MSVCRT__locale_t locale)
{
#ifdef PRINTF_WIDE
    return pf_puts(puts_ctx, len, str);
#else
    LPSTR out;
    int len_a = WideCharToMultiByte(locale->locinfo->lc_codepage, 0, str, len, NULL, 0, NULL, NULL);

    out = HeapAlloc(GetProcessHeap(), 0, len_a);
    if(!out)
        return -1;

    WideCharToMultiByte(locale->locinfo->lc_codepage, 0, str, len, out, len_a, NULL, NULL);
    len = pf_puts(puts_ctx, len_a, out);
    HeapFree(GetProcessHeap(), 0, out);
    return len;
#endif
}

static inline int FUNC_NAME(pf_output_str)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx,
        const char *str, int len, MSVCRT__locale_t locale)
{
#ifdef PRINTF_WIDE
    LPWSTR out;
    int len_w = MultiByteToWideChar(locale->locinfo->lc_codepage, 0, str, len, NULL, 0);

    out = HeapAlloc(GetProcessHeap(), 0, len_w*sizeof(WCHAR));
    if(!out)
        return -1;

    MultiByteToWideChar(locale->locinfo->lc_codepage, 0, str, len, out, len_w);
    len = pf_puts(puts_ctx, len_w, out);
    HeapFree(GetProcessHeap(), 0, out);
    return len;
#else
    return pf_puts(puts_ctx, len, str);
#endif
}

static inline int FUNC_NAME(pf_output_format_wstr)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx,
        const MSVCRT_wchar_t *str, int len, FUNC_NAME(pf_flags) *flags, MSVCRT__locale_t locale)
{
    int r, ret;

    if(len < 0)
        len = strlenW(str);

    if(flags->Precision>=0 && flags->Precision<len)
        len = flags->Precision;

    r = FUNC_NAME(pf_fill)(pf_puts, puts_ctx, len, flags, TRUE);
    ret = r;
    if(r >= 0) {
        r = FUNC_NAME(pf_output_wstr)(pf_puts, puts_ctx, str, len, locale);
        ret += r;
    }
    if(r >= 0) {
        r = FUNC_NAME(pf_fill)(pf_puts, puts_ctx, len, flags, FALSE);
        ret += r;
    }

    return r>=0 ? ret : r;
}

static inline int FUNC_NAME(pf_output_format_str)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx,
        const char *str, int len, FUNC_NAME(pf_flags) *flags, MSVCRT__locale_t locale)
{
    int r, ret;

    if(len < 0)
        len = strlen(str);

    if(flags->Precision>=0 && flags->Precision<len)
        len = flags->Precision;

    r = FUNC_NAME(pf_fill)(pf_puts, puts_ctx, len, flags, TRUE);
    ret = r;
    if(r >= 0) {
        r = FUNC_NAME(pf_output_str)(pf_puts, puts_ctx, str, len, locale);
        ret += r;
    }
    if(r >= 0) {
        r = FUNC_NAME(pf_fill)(pf_puts, puts_ctx, len, flags, FALSE);
        ret += r;
    }

    return r>=0 ? ret : r;
}

static inline int FUNC_NAME(pf_handle_string)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx,
        const void *str, int len, FUNC_NAME(pf_flags) *flags, MSVCRT__locale_t locale)
{
#ifdef PRINTF_WIDE
    static const MSVCRT_wchar_t nullW[] = {'(','n','u','l','l',')',0};

    if(!str)
        return FUNC_NAME(pf_output_format_wstr)(pf_puts, puts_ctx, nullW, 6, flags, locale);
#else
    if(!str)
        return FUNC_NAME(pf_output_format_str)(pf_puts, puts_ctx, "(null)", 6, flags, locale);
#endif

    if(flags->WideString || flags->IntegerLength=='l')
        return FUNC_NAME(pf_output_format_wstr)(pf_puts, puts_ctx, str, len, flags, locale);
    if(flags->IntegerLength == 'h')
        return FUNC_NAME(pf_output_format_str)(pf_puts, puts_ctx, str, len, flags, locale);

    if((flags->Format=='S' || flags->Format=='C') == (sizeof(APICHAR)==sizeof(MSVCRT_wchar_t)))
        return FUNC_NAME(pf_output_format_str)(pf_puts, puts_ctx, str, len, flags, locale);
    else
        return FUNC_NAME(pf_output_format_wstr)(pf_puts, puts_ctx, str, len, flags, locale);
}

static inline void FUNC_NAME(pf_rebuild_format_string)(char *p, FUNC_NAME(pf_flags) *flags)
{
    *p++ = '%';
    if(flags->Sign)
        *p++ = flags->Sign;
    if(flags->LeftAlign)
        *p++ = flags->LeftAlign;
    if(flags->Alternate)
        *p++ = flags->Alternate;
    if(flags->PadZero)
        *p++ = flags->PadZero;
    if(flags->FieldLength) {
        sprintf(p, "%d", flags->FieldLength);
        p += strlen(p);
    }
    if(flags->Precision >= 0) {
        sprintf(p, ".%d", flags->Precision);
        p += strlen(p);
    }
    *p++ = flags->Format;
    *p++ = 0;
}

/* pf_integer_conv:  prints x to buf, including alternate formats and
   additional precision digits, but not field characters or the sign */
static inline void FUNC_NAME(pf_integer_conv)(char *buf, int buf_len,
        FUNC_NAME(pf_flags) *flags, LONGLONG x)
{
    unsigned int base;
    const char *digits;

    int i, j, k;
    char number[40], *tmp = number;

    if(buf_len > sizeof number)
        tmp = HeapAlloc(GetProcessHeap(), 0, buf_len);

    if(flags->Format == 'o')
        base = 8;
    else if(flags->Format=='x' || flags->Format=='X')
        base = 16;
    else
        base = 10;

    if(flags->Format == 'X')
        digits = "0123456789ABCDEFX";
    else
        digits = "0123456789abcdefx";

    if(x<0 && (flags->Format=='d' || flags->Format=='i')) {
        x = -x;
        flags->Sign = '-';
    }

    i = 0;
    if(x==0 && flags->Precision)
        tmp[i++] = '0';
    else {
        while(x != 0) {
            j = (ULONGLONG)x%base;
            x = (ULONGLONG)x/base;
            tmp[i++] = digits[j];
        }
    }
    k = flags->Precision-i;
    while(k-- > 0)
        tmp[i++] = '0';
    if(flags->Alternate) {
        if(base == 16) {
            tmp[i++] = digits[16];
            tmp[i++] = '0';
        } else if(base==8 && tmp[i-1]!='0')
            tmp[i++] = '0';
    }

    j = 0;
    while(i-- > 0)
        buf[j++] = tmp[i];
    buf[j] = '\0';

    /* Adjust precision so pf_fill won't truncate the number later */
    flags->Precision = strlen(buf);

    if(tmp != number)
        HeapFree(GetProcessHeap(), 0, tmp);
}

static inline void FUNC_NAME(pf_fixup_exponent)(char *buf)
{
    char* tmp = buf;

    while(tmp[0] && toupper(tmp[0])!='E')
        tmp++;

    if(tmp[0] && (tmp[1]=='+' || tmp[1]=='-') &&
            isdigit(tmp[2]) && isdigit(tmp[3])) {
        char final;

        if (isdigit(tmp[4]))
            return; /* Exponent already 3 digits */

        tmp += 2;
        final = tmp[2];
        tmp[2] = tmp[1];
        tmp[1] = tmp[0];
        tmp[0] = '0';

        if(final == '\0') {
            tmp[3] = '\0';
            if(buf[0] == ' ')
                memmove(buf, buf + 1, (tmp - buf) + 3);
        }
    }
}

int FUNC_NAME(pf_printf)(FUNC_NAME(puts_clbk) pf_puts, void *puts_ctx, const APICHAR *fmt,
        MSVCRT__locale_t locale, BOOL positional_params, BOOL invoke_invalid_param_handler,
        args_clbk pf_args, void *args_ctx, __ms_va_list valist)
{
    const APICHAR *q, *p = fmt;
    int written = 0, pos, i;
    FUNC_NAME(pf_flags) flags;
    char buf[32];

    TRACE("Format is: %s\n", FUNC_NAME(debugstr)(fmt));

    if(!locale)
        locale = get_locale();

    while(*p) {
        /* output characters before '%' */
        for(q=p; *q && *q!='%'; q++);
        if(p != q) {
            i = pf_puts(puts_ctx, q-p, p);
            if(i < 0)
                return i;

            written += i;
            p = q;
            continue;
        }

        /* *p == '%' here */
        p++;

        /* output a single '%' character */
        if(*p == '%') {
            i = pf_puts(puts_ctx, 1, p++);
            if(i < 0)
                return i;

            written += i;
            continue;
        }

        /* check parameter position */
        if(positional_params && (q = FUNC_NAME(pf_parse_int)(p, &pos)) && *q=='$')
            p = q+1;
        else
            pos = -1;

        /* parse the flags */
        memset(&flags, 0, sizeof(flags));
        while(*p) {
            if(*p=='+' || *p==' ') {
                if(flags.Sign != '+')
                    flags.Sign = *p;
            } else if(*p == '-')
                flags.LeftAlign = *p;
            else if(*p == '0')
                flags.PadZero = *p;
            else if(*p == '#')
                flags.Alternate = *p;
            else
                break;

            p++;
        }

        /* parse the widh */
        if(*p == '*') {
            p++;
            if(positional_params && (q = FUNC_NAME(pf_parse_int)(p, &i)) && *q=='$')
                p = q+1;
            else
                i = -1;

            flags.FieldLength = pf_args(args_ctx, i, VT_INT, &valist).get_int;
            if(flags.FieldLength < 0) {
                flags.LeftAlign = '-';
                flags.FieldLength = -flags.FieldLength;
            }
        } else while(isdigit(*p)) {
            flags.FieldLength *= 10;
            flags.FieldLength += *p++ - '0';
        }

        /* parse the precision */
        flags.Precision = -1;
        if(*p == '.') {
            flags.Precision = 0;
            p++;
            if(*p == '*') {
                p++;
                if(positional_params && (q = FUNC_NAME(pf_parse_int)(p, &i)) && *q=='$')
                    p = q+1;
                else
                    i = -1;

                flags.Precision = pf_args(args_ctx, i, VT_INT, &valist).get_int;
            } else while(isdigit(*p)) {
                flags.Precision *= 10;
                flags.Precision += *p++ - '0';
            }
        }

        /* parse argument size modifier */
        while(*p) {
            if(*p=='l' && *(p+1)=='l') {
                flags.IntegerDouble++;
                p += 2;
            } else if(*p=='h' || *p=='l' || *p=='L') {
                flags.IntegerLength = *p;
                p++;
            } else if(*p == 'I') {
                if(*(p+1)=='6' && *(p+2)=='4') {
                    flags.IntegerDouble++;
                    p += 3;
                } else if(*(p+1)=='3' && *(p+2)=='2')
                    p += 3;
                else if(isdigit(*(p+1)) || !*(p+1))
                    break;
                else
                    p++;
            } else if(*p == 'w')
                flags.WideString = *p++;
            else if(*p == 'F')
                p++; /* ignore */
            else
                break;
        }

        flags.Format = *p;

        if(flags.Format == 's' || flags.Format == 'S') {
            i = FUNC_NAME(pf_handle_string)(pf_puts, puts_ctx,
                    pf_args(args_ctx, pos, VT_PTR, &valist).get_ptr,
                    -1,  &flags, locale);
        } else if(flags.Format == 'c' || flags.Format == 'C') {
            int ch = pf_args(args_ctx, pos, VT_INT, &valist).get_int;

            if((ch&0xff) != ch)
                FIXME("multibyte characters printing not supported\n");

            i = FUNC_NAME(pf_handle_string)(pf_puts, puts_ctx, &ch, 1, &flags, locale);
        } else if(flags.Format == 'p') {
            void *ptr = pf_args(args_ctx, pos, VT_PTR, &valist).get_ptr;

            flags.PadZero = 0;
            if(flags.Alternate)
                sprintf(buf, "0X%0*lX", 2*(int)sizeof(ptr), (ULONG_PTR)ptr);
            else
                sprintf(buf, "%0*lX", 2*(int)sizeof(ptr), (ULONG_PTR)ptr);

            i = FUNC_NAME(pf_output_format_str)(pf_puts, puts_ctx, buf, -1, &flags, locale);
        } else if(flags.Format == 'n') {
            int *used = pf_args(args_ctx, pos, VT_PTR, &valist).get_ptr;
            *used = written;
            i = 0;
        } else if(flags.IntegerDouble && flags.Format && strchr("diouxX", flags.Format)) {
            char *tmp = buf;
            int max_len = (flags.FieldLength>flags.Precision ? flags.FieldLength : flags.Precision) + 10;

            if(max_len > sizeof(buf))
                tmp = HeapAlloc(GetProcessHeap(), 0, max_len);
            if(!tmp)
                return -1;

            FUNC_NAME(pf_integer_conv)(tmp, max_len, &flags,
                    pf_args(args_ctx, pos, VT_I8, &valist).get_longlong);

            i = FUNC_NAME(pf_output_format_str)(pf_puts, puts_ctx, tmp, -1, &flags, locale);
            if(tmp != buf)
                HeapFree(GetProcessHeap(), 0, tmp);
        } else if(flags.Format && strchr("acCdeEfgGinouxX", flags.Format)) {
            char fmt[20], *tmp = buf, *decimal_point;
            int max_len = (flags.FieldLength>flags.Precision ? flags.FieldLength : flags.Precision) + 10;

            if(max_len > sizeof(buf))
                tmp = HeapAlloc(GetProcessHeap(), 0, max_len);
            if(!tmp)
                return -1;

            FUNC_NAME(pf_rebuild_format_string)(fmt, &flags);

            if(flags.Format && strchr("aeEfgG", flags.Format)) {
                sprintf(tmp, fmt, pf_args(args_ctx, pos, VT_R8, &valist).get_double);
                if(toupper(flags.Format)=='E' || toupper(flags.Format)=='G')
                    FUNC_NAME(pf_fixup_exponent)(tmp);
            }
            else
                sprintf(tmp, fmt, pf_args(args_ctx, pos, VT_INT, &valist).get_int);

            decimal_point = strchr(tmp, '.');
            if(decimal_point)
                *decimal_point = *locale->locinfo->lconv->decimal_point;

            i = FUNC_NAME(pf_output_str)(pf_puts, puts_ctx, tmp, strlen(tmp), locale);
            if(tmp != buf)
                HeapFree(GetProcessHeap(), 0, tmp);
        } else {
            if(invoke_invalid_param_handler) {
                MSVCRT__invalid_parameter(NULL, NULL, NULL, 0, 0);
                *MSVCRT__errno() = MSVCRT_EINVAL;
                return -1;
            }

            continue;
        }

        if(i < 0)
            return i;
        written += i;
        p++;
    }

    return written;
}

#undef APICHAR
#undef CONVCHAR
#undef FUNC_NAME
