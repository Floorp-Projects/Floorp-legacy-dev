/* alg1485.c - implementation of RFCs 1485, 1779 and 2253.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <limits.h>
#include "prprf.h"
#include "cert.h"
#include "certi.h"
#include "xconst.h"
#include "genname.h"
#include "secitem.h"
#include "secerr.h"

typedef struct NameToKindStr {
    const char* name;
    unsigned int maxLen; /* max bytes in UTF8 encoded string value */
    SECOidTag kind;
    int valueType;
} NameToKind;

/* local type for directory string--could be printable_string or utf8 */
#define SEC_ASN1_DS SEC_ASN1_HIGH_TAG_NUMBER

/* clang-format off */

/* Add new entries to this table, and maybe to function ParseRFC1485AVA */
static const NameToKind name2kinds[] = {
/* IANA registered type names
 * (See: http://www.iana.org/assignments/ldap-parameters)
 */
/* RFC 3280, 4630 MUST SUPPORT */
    { "CN",            640, SEC_OID_AVA_COMMON_NAME,    SEC_ASN1_DS},
    { "ST",            128, SEC_OID_AVA_STATE_OR_PROVINCE,
                                                        SEC_ASN1_DS},
    { "O",             128, SEC_OID_AVA_ORGANIZATION_NAME,
                                                        SEC_ASN1_DS},
    { "OU",            128, SEC_OID_AVA_ORGANIZATIONAL_UNIT_NAME,
                                                        SEC_ASN1_DS},
    { "dnQualifier", 32767, SEC_OID_AVA_DN_QUALIFIER, SEC_ASN1_PRINTABLE_STRING},
    { "C",               2, SEC_OID_AVA_COUNTRY_NAME, SEC_ASN1_PRINTABLE_STRING},
    { "serialNumber",   64, SEC_OID_AVA_SERIAL_NUMBER,SEC_ASN1_PRINTABLE_STRING},

/* RFC 3280, 4630 SHOULD SUPPORT */
    { "L",             128, SEC_OID_AVA_LOCALITY,       SEC_ASN1_DS},
    { "title",          64, SEC_OID_AVA_TITLE,          SEC_ASN1_DS},
    { "SN",             64, SEC_OID_AVA_SURNAME,        SEC_ASN1_DS},
    { "givenName",      64, SEC_OID_AVA_GIVEN_NAME,     SEC_ASN1_DS},
    { "initials",       64, SEC_OID_AVA_INITIALS,       SEC_ASN1_DS},
    { "generationQualifier",
                        64, SEC_OID_AVA_GENERATION_QUALIFIER,
                                                        SEC_ASN1_DS},
/* RFC 3280, 4630 MAY SUPPORT */
    { "DC",            128, SEC_OID_AVA_DC,             SEC_ASN1_IA5_STRING},
    { "MAIL",          256, SEC_OID_RFC1274_MAIL,       SEC_ASN1_IA5_STRING},
    { "UID",           256, SEC_OID_RFC1274_UID,        SEC_ASN1_DS},

/* ------------------ "strict" boundary ---------------------------------
 * In strict mode, cert_NameToAscii does not encode any of the attributes
 * below this line. The first SECOidTag below this line must be used to
 * conditionally define the "endKind" in function AppendAVA() below.
 * Most new attribute names should be added below this line.
 * Maybe this line should be up higher?  Say, after the 3280 MUSTs and
 * before the 3280 SHOULDs?
 */

/* values from draft-ietf-ldapbis-user-schema-05 (not in RFC 3280) */
    { "postalAddress", 128, SEC_OID_AVA_POSTAL_ADDRESS, SEC_ASN1_DS},
    { "postalCode",     40, SEC_OID_AVA_POSTAL_CODE,    SEC_ASN1_DS},
    { "postOfficeBox",  40, SEC_OID_AVA_POST_OFFICE_BOX,SEC_ASN1_DS},
    { "houseIdentifier",64, SEC_OID_AVA_HOUSE_IDENTIFIER,SEC_ASN1_DS},
/* end of IANA registered type names */

/* legacy keywords */
    { "E",             128, SEC_OID_PKCS9_EMAIL_ADDRESS,SEC_ASN1_IA5_STRING},
    { "STREET",        128, SEC_OID_AVA_STREET_ADDRESS, SEC_ASN1_DS},
    { "pseudonym",      64, SEC_OID_AVA_PSEUDONYM,      SEC_ASN1_DS},

/* values defined by the CAB Forum for EV */
    { "incorporationLocality", 128, SEC_OID_EV_INCORPORATION_LOCALITY,
                                    SEC_ASN1_DS},
    { "incorporationState",    128, SEC_OID_EV_INCORPORATION_STATE,
                                    SEC_ASN1_DS},
    { "incorporationCountry",    2, SEC_OID_EV_INCORPORATION_COUNTRY,
                                    SEC_ASN1_PRINTABLE_STRING},
    { "businessCategory",       64, SEC_OID_BUSINESS_CATEGORY, SEC_ASN1_DS},

/* values defined in X.520 */
    { "name",           64, SEC_OID_AVA_NAME,           SEC_ASN1_DS},

    { 0,               256, SEC_OID_UNKNOWN,            0},
};

/* Table facilitates conversion of ASCII hex to binary. */
static const PRInt16 x2b[256] = {
/* #0x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #1x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #2x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #3x */  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
/* #4x */ -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #5x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #6x */ -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #7x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #8x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #9x */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #ax */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #bx */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #cx */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #dx */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #ex */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/* #fx */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

#define IS_HEX(c) (x2b[(PRUint8)(c)] >= 0)

#define C_DOUBLE_QUOTE '\042'

#define C_BACKSLASH '\134'

#define C_EQUAL '='

#define OPTIONAL_SPACE(c)                                                      \
    (((c) == ' ') || ((c) == '\r') || ((c) == '\n'))

#define SPECIAL_CHAR(c)                                                        \
    (((c) == ',') || ((c) == '=') || ((c) == C_DOUBLE_QUOTE) ||                \
     ((c) == '\r') || ((c) == '\n') || ((c) == '+') ||                         \
     ((c) == '<') || ((c) == '>') || ((c) == '#') ||                           \
     ((c) == ';') || ((c) == C_BACKSLASH))


#define IS_PRINTABLE(c)                                                        \
    ((((c) >= 'a') && ((c) <= 'z')) ||                                         \
     (((c) >= 'A') && ((c) <= 'Z')) ||                                         \
     (((c) >= '0') && ((c) <= '9')) ||                                         \
     ((c) == ' ') ||                                                           \
     ((c) == '\'') ||                                                          \
     ((c) == '\050') ||                     /* ( */                            \
     ((c) == '\051') ||                     /* ) */                            \
     (((c) >= '+') && ((c) <= '/')) ||      /* + , - . / */                    \
     ((c) == ':') ||                                                           \
     ((c) == '=') ||                                                           \
     ((c) == '?'))

/* clang-format on */

/* RFC 2253 says we must escape ",+\"\\<>;=" EXCEPT inside a quoted string.
 * Inside a quoted string, we only need to escape " and \
 * We choose to quote strings containing any of those special characters,
 * so we only need to escape " and \
 */
#define NEEDS_ESCAPE(c) (c == C_DOUBLE_QUOTE || c == C_BACKSLASH)

#define NEEDS_HEX_ESCAPE(c) ((PRUint8)c < 0x20 || c == 0x7f)

int
cert_AVAOidTagToMaxLen(SECOidTag tag)
{
    const NameToKind* n2k = name2kinds;

    while (n2k->kind != tag && n2k->kind != SEC_OID_UNKNOWN) {
        ++n2k;
    }
    return (n2k->kind != SEC_OID_UNKNOWN) ? n2k->maxLen : -1;
}

static PRBool
IsPrintable(unsigned char* data, unsigned len)
{
    unsigned char ch, *end;

    end = data + len;
    while (data < end) {
        ch = *data++;
        if (!IS_PRINTABLE(ch)) {
            return PR_FALSE;
        }
    }
    return PR_TRUE;
}

static void
skipSpace(const char** pbp, const char* endptr)
{
    const char* bp = *pbp;
    while (bp < endptr && OPTIONAL_SPACE(*bp)) {
        bp++;
    }
    *pbp = bp;
}

static SECStatus
scanTag(const char** pbp, const char* endptr, char* tagBuf, int tagBufSize)
{
    const char* bp;
    char* tagBufp;
    int taglen;

    PORT_Assert(tagBufSize > 0);

    /* skip optional leading space */
    skipSpace(pbp, endptr);
    if (*pbp == endptr) {
        /* nothing left */
        return SECFailure;
    }

    /* fill tagBuf */
    taglen = 0;
    bp = *pbp;
    tagBufp = tagBuf;
    while (bp < endptr && !OPTIONAL_SPACE(*bp) && (*bp != C_EQUAL)) {
        if (++taglen >= tagBufSize) {
            *pbp = bp;
            return SECFailure;
        }
        *tagBufp++ = *bp++;
    }
    /* null-terminate tagBuf -- guaranteed at least one space left */
    *tagBufp++ = 0;
    *pbp = bp;

    /* skip trailing spaces till we hit something - should be an equal sign */
    skipSpace(pbp, endptr);
    if (*pbp == endptr) {
        /* nothing left */
        return SECFailure;
    }
    if (**pbp != C_EQUAL) {
        /* should be an equal sign */
        return SECFailure;
    }
    /* skip over the equal sign */
    (*pbp)++;

    return SECSuccess;
}

/* Returns the number of bytes in the value. 0 means failure. */
static int
scanVal(const char** pbp, const char* endptr, char* valBuf, int valBufSize)
{
    const char* bp;
    char* valBufp;
    int vallen = 0;
    PRBool isQuoted;

    PORT_Assert(valBufSize > 0);

    /* skip optional leading space */
    skipSpace(pbp, endptr);
    if (*pbp == endptr) {
        /* nothing left */
        return 0;
    }

    bp = *pbp;

    /* quoted? */
    if (*bp == C_DOUBLE_QUOTE) {
        isQuoted = PR_TRUE;
        /* skip over it */
        bp++;
    } else {
        isQuoted = PR_FALSE;
    }

    valBufp = valBuf;
    while (bp < endptr) {
        char c = *bp;
        if (c == C_BACKSLASH) {
            /* escape character */
            bp++;
            if (bp >= endptr) {
                /* escape charater must appear with paired char */
                *pbp = bp;
                return 0;
            }
            c = *bp;
            if (IS_HEX(c) && (endptr - bp) >= 2 && IS_HEX(bp[1])) {
                bp++;
                c = (char)((x2b[(PRUint8)c] << 4) | x2b[(PRUint8)*bp]);
            }
        } else if (c == '#' && bp == *pbp) {
            /* ignore leading #, quotation not required for it. */
        } else if (!isQuoted && SPECIAL_CHAR(c)) {
            /* unescaped special and not within quoted value */
            break;
        } else if (c == C_DOUBLE_QUOTE) {
            /* reached unescaped double quote */
            break;
        }
        /* append character */
        vallen++;
        if (vallen >= valBufSize) {
            *pbp = bp;
            return 0;
        }
        *valBufp++ = c;
        bp++;
    }

    /* strip trailing spaces from unquoted values */
    if (!isQuoted) {
        while (valBufp > valBuf) {
            char c = valBufp[-1];
            if (!OPTIONAL_SPACE(c))
                break;
            --valBufp;
        }
        vallen = valBufp - valBuf;
    }

    if (isQuoted) {
        /* insist that we stopped on a double quote */
        if (*bp != C_DOUBLE_QUOTE) {
            *pbp = bp;
            return 0;
        }
        /* skip over the quote and skip optional space */
        bp++;
        skipSpace(&bp, endptr);
    }

    *pbp = bp;

    /* null-terminate valBuf -- guaranteed at least one space left */
    *valBufp = 0;

    return vallen;
}

/* Caller must set error code upon failure */
static SECStatus
hexToBin(PLArenaPool* pool, SECItem* destItem, const char* src, int len)
{
    PRUint8* dest;

    destItem->data = NULL;
    if (len <= 0 || (len & 1)) {
        goto loser;
    }
    len >>= 1;
    if (!SECITEM_AllocItem(pool, destItem, len)) {
        goto loser;
    }
    dest = destItem->data;
    for (; len > 0; len--, src += 2) {
        PRUint16 bin = ((PRUint16)x2b[(PRUint8)src[0]] << 4);
        bin |= (PRUint16)x2b[(PRUint8)src[1]];
        if (bin >> 15) { /* is negative */
            goto loser;
        }
        *dest++ = (PRUint8)bin;
    }
    return SECSuccess;
loser:
    if (!pool)
        SECITEM_FreeItem(destItem, PR_FALSE);
    return SECFailure;
}

/* Parses one AVA, starting at *pbp.  Stops at endptr.
 * Advances *pbp past parsed AVA and trailing separator (if present).
 * On any error, returns NULL and *pbp is undefined.
 * On success, returns CERTAVA allocated from arena, and (*pbp)[-1] was
 * the last character parsed.  *pbp is either equal to endptr or
 * points to first character after separator.
 */
static CERTAVA*
ParseRFC1485AVA(PLArenaPool* arena, const char** pbp, const char* endptr)
{
    CERTAVA* a;
    const NameToKind* n2k;
    const char* bp;
    int vt = -1;
    int valLen;
    PRBool isDottedOid = PR_FALSE;
    SECOidTag kind = SEC_OID_UNKNOWN;
    SECStatus rv = SECFailure;
    SECItem derOid = { 0, NULL, 0 };
    SECItem derVal = { 0, NULL, 0 };
    char sep = 0;

    char tagBuf[32];
    char valBuf[1024];

    PORT_Assert(arena);
    if (SECSuccess != scanTag(pbp, endptr, tagBuf, sizeof tagBuf) ||
        !(valLen = scanVal(pbp, endptr, valBuf, sizeof valBuf))) {
        goto loser;
    }

    bp = *pbp;
    if (bp < endptr) {
        sep = *bp++; /* skip over separator */
    }
    *pbp = bp;
    /* if we haven't finished, insist that we've stopped on a separator */
    if (sep && sep != ',' && sep != ';' && sep != '+') {
        goto loser;
    }

    /* is this a dotted decimal OID attribute type ? */
    if (!PL_strncasecmp("oid.", tagBuf, 4) || isdigit(tagBuf[0])) {
        rv = SEC_StringToOID(arena, &derOid, tagBuf, strlen(tagBuf));
        isDottedOid = (PRBool)(rv == SECSuccess);
    } else {
        for (n2k = name2kinds; n2k->name; n2k++) {
            SECOidData* oidrec;
            if (PORT_Strcasecmp(n2k->name, tagBuf) == 0) {
                kind = n2k->kind;
                vt = n2k->valueType;
                oidrec = SECOID_FindOIDByTag(kind);
                if (oidrec == NULL)
                    goto loser;
                derOid = oidrec->oid;
                break;
            }
        }
    }
    if (kind == SEC_OID_UNKNOWN && rv != SECSuccess)
        goto loser;

    /* Is this a hex encoding of a DER attribute value ? */
    if ('#' == valBuf[0]) {
        /* convert attribute value from hex to binary */
        rv = hexToBin(arena, &derVal, valBuf + 1, valLen - 1);
        if (rv)
            goto loser;
        a = CERT_CreateAVAFromRaw(arena, &derOid, &derVal);
    } else {
        if (kind == SEC_OID_AVA_COUNTRY_NAME && valLen != 2)
            goto loser;
        if (vt == SEC_ASN1_PRINTABLE_STRING &&
            !IsPrintable((unsigned char*)valBuf, valLen))
            goto loser;
        if (vt == SEC_ASN1_DS) {
            /* RFC 4630: choose PrintableString or UTF8String */
            if (IsPrintable((unsigned char*)valBuf, valLen))
                vt = SEC_ASN1_PRINTABLE_STRING;
            else
                vt = SEC_ASN1_UTF8_STRING;
        }

        derVal.data = (unsigned char*)valBuf;
        derVal.len = valLen;
        if (kind == SEC_OID_UNKNOWN && isDottedOid) {
            a = CERT_CreateAVAFromRaw(arena, &derOid, &derVal);
        } else {
            a = CERT_CreateAVAFromSECItem(arena, kind, vt, &derVal);
        }
    }
    return a;

loser:
    /* matched no kind -- invalid tag */
    PORT_SetError(SEC_ERROR_INVALID_AVA);
    return 0;
}

static CERTName*
ParseRFC1485Name(const char* buf, int len)
{
    SECStatus rv;
    CERTName* name;
    const char *bp, *e;
    CERTAVA* ava;
    CERTRDN* rdn = NULL;

    name = CERT_CreateName(NULL);
    if (name == NULL) {
        return NULL;
    }

    e = buf + len;
    bp = buf;
    while (bp < e) {
        ava = ParseRFC1485AVA(name->arena, &bp, e);
        if (ava == 0)
            goto loser;
        if (!rdn) {
            rdn = CERT_CreateRDN(name->arena, ava, (CERTAVA*)0);
            if (rdn == 0)
                goto loser;
            rv = CERT_AddRDN(name, rdn);
        } else {
            rv = CERT_AddAVA(name->arena, rdn, ava);
        }
        if (rv)
            goto loser;
        if (bp[-1] != '+')
            rdn = NULL; /* done with this RDN */
        skipSpace(&bp, e);
    }

    if (name->rdns[0] == 0) {
        /* empty name -- illegal */
        goto loser;
    }

    /* Reverse order of RDNS to comply with RFC */
    {
        CERTRDN** firstRdn;
        CERTRDN** lastRdn;
        CERTRDN* tmp;

        /* get first one */
        firstRdn = name->rdns;

        /* find last one */
        lastRdn = name->rdns;
        while (*lastRdn)
            lastRdn++;
        lastRdn--;

        /* reverse list */
        for (; firstRdn < lastRdn; firstRdn++, lastRdn--) {
            tmp = *firstRdn;
            *firstRdn = *lastRdn;
            *lastRdn = tmp;
        }
    }

    /* return result */
    return name;

loser:
    CERT_DestroyName(name);
    return NULL;
}

CERTName*
CERT_AsciiToName(const char* string)
{
    CERTName* name;
    name = ParseRFC1485Name(string, PORT_Strlen(string));
    return name;
}

/************************************************************************/

typedef struct stringBufStr {
    char* buffer;
    unsigned offset;
    unsigned size;
} stringBuf;

#define DEFAULT_BUFFER_SIZE 200

static SECStatus
AppendStr(stringBuf* bufp, char* str)
{
    char* buf;
    unsigned bufLen, bufSize, len;
    int size = 0;

    /* Figure out how much to grow buf by (add in the '\0') */
    buf = bufp->buffer;
    bufLen = bufp->offset;
    len = PORT_Strlen(str);
    bufSize = bufLen + len;
    if (!buf) {
        bufSize++;
        size = PR_MAX(DEFAULT_BUFFER_SIZE, bufSize * 2);
        buf = (char*)PORT_Alloc(size);
        bufp->size = size;
    } else if (bufp->size < bufSize) {
        size = bufSize * 2;
        buf = (char*)PORT_Realloc(buf, size);
        bufp->size = size;
    }
    if (!buf) {
        PORT_SetError(SEC_ERROR_NO_MEMORY);
        return SECFailure;
    }
    bufp->buffer = buf;
    bufp->offset = bufSize;

    /* Concatenate str onto buf */
    buf = buf + bufLen;
    if (bufLen)
        buf--;                      /* stomp on old '\0' */
    PORT_Memcpy(buf, str, len + 1); /* put in new null */
    return SECSuccess;
}

typedef enum {
    minimalEscape = 0,     /* only hex escapes, and " and \ */
    minimalEscapeAndQuote, /* as above, plus quoting        */
    fullEscape             /* no quoting, full escaping     */
} EQMode;

/* Some characters must be escaped as a hex string, e.g. c -> \nn .
 * Others must be escaped by preceding with a '\', e.g. c -> \c , but
 * there are certain "special characters" that may be handled by either
 * escaping them, or by enclosing the entire attribute value in quotes.
 * A NULL value for pEQMode implies selecting minimalEscape mode.
 * Some callers will do quoting when needed, others will not.
 * If a caller selects minimalEscapeAndQuote, and the string does not
 * need quoting, then this function changes it to minimalEscape.
 * Limit source to 16K, which avoids any possibility of overflow.
 * Maximum output size would be 3*srclen+2.
 */
static int
cert_RFC1485_GetRequiredLen(const char* src, int srclen, EQMode* pEQMode)
{
    int i, reqLen = 0;
    EQMode mode = pEQMode ? *pEQMode : minimalEscape;
    PRBool needsQuoting = PR_FALSE;
    char lastC = 0;

    /* avoids needing to check for overflow */
    if (srclen > 16384) {
        return -1;
    }
    /* need to make an initial pass to determine if quoting is needed */
    for (i = 0; i < srclen; i++) {
        char c = src[i];
        reqLen++;
        if (NEEDS_HEX_ESCAPE(c)) { /* c -> \xx  */
            reqLen += 2;
        } else if (NEEDS_ESCAPE(c)) { /* c -> \c   */
            reqLen++;
        } else if (SPECIAL_CHAR(c)) {
            if (mode == minimalEscapeAndQuote) /* quoting is allowed */
                needsQuoting = PR_TRUE;        /* entirety will need quoting */
            else if (mode == fullEscape)
                reqLen++; /* MAY escape this character */
        } else if (OPTIONAL_SPACE(c) && OPTIONAL_SPACE(lastC)) {
            if (mode == minimalEscapeAndQuote) /* quoting is allowed */
                needsQuoting = PR_TRUE;        /* entirety will need quoting */
        }
        lastC = c;
    }
    /* if it begins or ends in optional space it needs quoting */
    if (!needsQuoting && srclen > 0 && mode == minimalEscapeAndQuote &&
        (OPTIONAL_SPACE(src[srclen - 1]) || OPTIONAL_SPACE(src[0]))) {
        needsQuoting = PR_TRUE;
    }

    if (needsQuoting)
        reqLen += 2;
    if (pEQMode && mode == minimalEscapeAndQuote && !needsQuoting)
        *pEQMode = minimalEscape;
    /* Maximum output size would be 3*srclen+2 */
    return reqLen;
}

static const char hexChars[16] = { "0123456789abcdef" };

static SECStatus
escapeAndQuote(char* dst, int dstlen, char* src, int srclen, EQMode* pEQMode)
{
    int i, reqLen = 0;
    EQMode mode = pEQMode ? *pEQMode : minimalEscape;

    reqLen = cert_RFC1485_GetRequiredLen(src, srclen, &mode);
    /* reqLen is max 16384*3 + 2 */
    /* space for terminal null */
    if (reqLen < 0 || reqLen + 1 > dstlen) {
        PORT_SetError(SEC_ERROR_OUTPUT_LEN);
        return SECFailure;
    }
    reqLen += 1;

    if (mode == minimalEscapeAndQuote)
        *dst++ = C_DOUBLE_QUOTE;
    for (i = 0; i < srclen; i++) {
        char c = src[i];
        if (NEEDS_HEX_ESCAPE(c)) {
            *dst++ = C_BACKSLASH;
            *dst++ = hexChars[(c >> 4) & 0x0f];
            *dst++ = hexChars[c & 0x0f];
        } else {
            if (NEEDS_ESCAPE(c) || (SPECIAL_CHAR(c) && mode == fullEscape)) {
                *dst++ = C_BACKSLASH;
            }
            *dst++ = c;
        }
    }
    if (mode == minimalEscapeAndQuote)
        *dst++ = C_DOUBLE_QUOTE;
    *dst++ = 0;
    if (pEQMode)
        *pEQMode = mode;
    return SECSuccess;
}

SECStatus
CERT_RFC1485_EscapeAndQuote(char* dst, int dstlen, char* src, int srclen)
{
    EQMode mode = minimalEscapeAndQuote;
    return escapeAndQuote(dst, dstlen, src, srclen, &mode);
}

/* convert an OID to dotted-decimal representation */
/* Returns a string that must be freed with PR_smprintf_free(), */
char*
CERT_GetOidString(const SECItem* oid)
{
    PRUint8* stop;  /* points to first byte after OID string */
    PRUint8* first; /* byte of an OID component integer      */
    PRUint8* last;  /* byte of an OID component integer      */
    char* rvString = NULL;
    char* prefix = NULL;

#define MAX_OID_LEN 1024 /* bytes */

    if (oid->len > MAX_OID_LEN) {
        PORT_SetError(SEC_ERROR_INPUT_LEN);
        return NULL;
    }

    /* If the OID has length 1, we bail. */
    if (oid->len < 2) {
        return NULL;
    }

    /* first will point to the next sequence of bytes to decode */
    first = (PRUint8*)oid->data;
    /* stop points to one past the legitimate data */
    stop = &first[oid->len];

    /*
     * Check for our pseudo-encoded single-digit OIDs
     */
    if ((*first == 0x80) && (2 == oid->len)) {
        /* Funky encoding.  The second byte is the number */
        rvString = PR_smprintf("%lu", (PRUint32)first[1]);
        if (!rvString) {
            PORT_SetError(SEC_ERROR_NO_MEMORY);
        }
        return rvString;
    }

    for (; first < stop; first = last + 1) {
        unsigned int bytesBeforeLast;

        for (last = first; last < stop; last++) {
            if (0 == (*last & 0x80)) {
                break;
            }
        }
        /* There's no first bit set, so this isn't valid. Bail.*/
        if (last == stop) {
            goto unsupported;
        }
        bytesBeforeLast = (unsigned int)(last - first);
        if (bytesBeforeLast <= 3U) { /* 0-28 bit number */
            PRUint32 n = 0;
            PRUint32 c;

#define CGET(i, m)    \
    c = last[-i] & m; \
    n |= c << (7 * i)

#define CASE(i, m)  \
    case i:         \
        CGET(i, m); \
        if (!n)     \
        goto unsupported /* fall-through */

            switch (bytesBeforeLast) {
                CASE(3, 0x7f);
                CASE(2, 0x7f);
                CASE(1, 0x7f);
                case 0:
                    n |= last[0] & 0x7f;
                    break;
            }
            if (last[0] & 0x80) {
                goto unsupported;
            }

            if (!rvString) {
                /* This is the first number.. decompose it */
                PRUint32 one = PR_MIN(n / 40, 2); /* never > 2 */
                PRUint32 two = n - (one * 40);

                rvString = PR_smprintf("OID.%lu.%lu", one, two);
            } else {
                prefix = rvString;
                rvString = PR_smprintf("%s.%lu", prefix, n);
            }
        } else if (bytesBeforeLast <= 9U) { /* 29-64 bit number */
            PRUint64 n = 0;
            PRUint64 c;

            switch (bytesBeforeLast) {
                CASE(9, 0x01);
                CASE(8, 0x7f);
                CASE(7, 0x7f);
                CASE(6, 0x7f);
                CASE(5, 0x7f);
                CASE(4, 0x7f);
                CGET(3, 0x7f);
                CGET(2, 0x7f);
                CGET(1, 0x7f);
                CGET(0, 0x7f);
                break;
            }
            if (last[0] & 0x80)
                goto unsupported;

            if (!rvString) {
                /* This is the first number.. decompose it */
                PRUint64 one = PR_MIN(n / 40, 2); /* never > 2 */
                PRUint64 two = n - (one * 40);

                rvString = PR_smprintf("OID.%llu.%llu", one, two);
            } else {
                prefix = rvString;
                rvString = PR_smprintf("%s.%llu", prefix, n);
            }
        } else {
        /* More than a 64-bit number, or not minimal encoding. */
        unsupported:
            if (!rvString)
                rvString = PR_smprintf("OID.UNSUPPORTED");
            else {
                prefix = rvString;
                rvString = PR_smprintf("%s.UNSUPPORTED", prefix);
            }
        }

        if (prefix) {
            PR_smprintf_free(prefix);
            prefix = NULL;
        }
        if (!rvString) {
            PORT_SetError(SEC_ERROR_NO_MEMORY);
            break;
        }
    }
    return rvString;
}

/* convert DER-encoded hex to a string */
static SECItem*
get_hex_string(SECItem* data)
{
    SECItem* rv;
    unsigned int i, j;
    static const char hex[] = { "0123456789ABCDEF" };

    /* '#' + 2 chars per octet + terminator */
    rv = SECITEM_AllocItem(NULL, NULL, data->len * 2 + 2);
    if (!rv) {
        return NULL;
    }
    rv->data[0] = '#';
    rv->len = 1 + 2 * data->len;
    for (i = 0; i < data->len; i++) {
        j = data->data[i];
        rv->data[2 * i + 1] = hex[j >> 4];
        rv->data[2 * i + 2] = hex[j & 15];
    }
    rv->data[rv->len] = 0;
    return rv;
}

/* For compliance with RFC 2253, RFC 3280 and RFC 4630, we choose to
 * use the NAME=STRING form, rather than the OID.N.N=#hexXXXX form,
 * when both of these conditions are met:
 *  1) The attribute name OID (kind) has a known name string that is
 *     defined in one of those RFCs, or in RFCs that they cite, AND
 *  2) The attribute's value encoding is RFC compliant for the kind
 *     (e.g., the value's encoding tag is correct for the kind, and
 *     the value's length is in the range allowed for the kind, and
 *     the value's contents are appropriate for the encoding tag).
 *  Otherwise, we use the OID.N.N=#hexXXXX form.
 *
 *  If the caller prefers maximum human readability to RFC compliance,
 *  then
 *  - We print the kind in NAME= string form if we know the name
 *    string for the attribute type OID, regardless of whether the
 *    value is correctly encoded or not. else we use the OID.N.N= form.
 *  - We use the non-hex STRING form for the attribute value if the
 *    value can be represented in such a form.  Otherwise, we use
 *    the hex string form.
 *  This implies that, for maximum human readability, in addition to
 *  the two forms allowed by the RFC, we allow two other forms of output:
 *  - the OID.N.N=STRING form, and
 *  - the NAME=#hexXXXX form
 *  When the caller prefers maximum human readability, we do not allow
 *  the value of any attribute to exceed the length allowed by the RFC.
 *  If the attribute value exceeds the allowed length, we truncate it to
 *  the allowed length and append "...".
 *  Also in this case, we arbitrarily impose a limit on the length of the
 *  entire AVA encoding, regardless of the form, of 384 bytes per AVA.
 *  This limit includes the trailing NULL character.  If the encoded
 *  AVA length exceeds that limit, this function reports failure to encode
 *  the AVA.
 *
 *  An ASCII representation of an AVA is said to be "invertible" if
 *  conversion back to DER reproduces the original DER encoding exactly.
 *  The RFC 2253 rules do not ensure that all ASCII AVAs derived according
 *  to its rules are invertible. That is because the RFCs allow some
 *  attribute values to be encoded in any of a number of encodings,
 *  and the encoding type information is lost in the non-hex STRING form.
 *  This is particularly true of attributes of type DirectoryString.
 *  The encoding type information is always preserved in the hex string
 *  form, because the hex includes the entire DER encoding of the value.
 *
 *  So, when the caller perfers maximum invertibility, we apply the
 *  RFC compliance rules stated above, and add a third required
 *  condition on the use of the NAME=STRING form.
 *   3) The attribute's kind is not is allowed to be encoded in any of
 *      several different encodings, such as DirectoryStrings.
 *
 * The chief difference between CERT_N2A_STRICT and CERT_N2A_INVERTIBLE
 * is that the latter forces DirectoryStrings to be hex encoded.
 *
 * As a simplification, we assume the value is correctly encoded for
 * its encoding type.  That is, we do not test that all the characters
 * in a string encoded type are allowed by that type.  We assume it.
 */
static SECStatus
AppendAVA(stringBuf* bufp, CERTAVA* ava, CertStrictnessLevel strict)
{
#define TMPBUF_LEN 2048
    const NameToKind* pn2k = name2kinds;
    SECItem* avaValue = NULL;
    char* unknownTag = NULL;
    char* encodedAVA = NULL;
    PRBool useHex = PR_FALSE; /* use =#hexXXXX form */
    PRBool truncateName = PR_FALSE;
    PRBool truncateValue = PR_FALSE;
    SECOidTag endKind;
    SECStatus rv;
    unsigned int len;
    unsigned int nameLen, valueLen;
    unsigned int maxName, maxValue;
    EQMode mode = minimalEscapeAndQuote;
    NameToKind n2k = { NULL, 32767, SEC_OID_UNKNOWN, SEC_ASN1_DS };
    char tmpBuf[TMPBUF_LEN];

#define tagName n2k.name /* non-NULL means use NAME= form */
#define maxBytes n2k.maxLen
#define tag n2k.kind
#define vt n2k.valueType

    /* READABLE mode recognizes more names from the name2kinds table
   * than do STRICT or INVERTIBLE modes.  This assignment chooses the
   * point in the table where the attribute type name scanning stops.
   */
    endKind = (strict == CERT_N2A_READABLE) ? SEC_OID_UNKNOWN
                                            : SEC_OID_AVA_POSTAL_ADDRESS;
    tag = CERT_GetAVATag(ava);
    while (pn2k->kind != tag && pn2k->kind != endKind) {
        ++pn2k;
    }

    if (pn2k->kind != endKind) {
        n2k = *pn2k;
    } else if (strict != CERT_N2A_READABLE) {
        useHex = PR_TRUE;
    }
    /* For invertable form, force Directory Strings to use hex form. */
    if (strict == CERT_N2A_INVERTIBLE && vt == SEC_ASN1_DS) {
        tagName = NULL;   /* must use OID.N form */
        useHex = PR_TRUE; /* must use hex string */
    }
    if (!useHex) {
        avaValue = CERT_DecodeAVAValue(&ava->value);
        if (!avaValue) {
            useHex = PR_TRUE;
            if (strict != CERT_N2A_READABLE) {
                tagName = NULL; /* must use OID.N form */
            }
        }
    }
    if (!tagName) {
        /* handle unknown attribute types per RFC 2253 */
        tagName = unknownTag = CERT_GetOidString(&ava->type);
        if (!tagName) {
            if (avaValue)
                SECITEM_FreeItem(avaValue, PR_TRUE);
            return SECFailure;
        }
    }
    if (useHex) {
        avaValue = get_hex_string(&ava->value);
        if (!avaValue) {
            if (unknownTag)
                PR_smprintf_free(unknownTag);
            return SECFailure;
        }
    }

    nameLen = strlen(tagName);

    if (useHex) {
        valueLen = avaValue->len;
    } else {
        int reqLen = cert_RFC1485_GetRequiredLen((char*)avaValue->data, avaValue->len, &mode);
        if (reqLen < 0) {
            SECITEM_FreeItem(avaValue, PR_TRUE);
            return SECFailure;
        }
        valueLen = reqLen;
    }
    if (UINT_MAX - nameLen < 2 ||
        valueLen > UINT_MAX - nameLen - 2) {
        SECITEM_FreeItem(avaValue, PR_TRUE);
        return SECFailure;
    }
    len = nameLen + valueLen + 2; /* Add 2 for '=' and trailing NUL */

    maxName = nameLen;
    maxValue = valueLen;
    if (len <= sizeof(tmpBuf)) {
        encodedAVA = tmpBuf;
    } else if (strict != CERT_N2A_READABLE) {
        encodedAVA = PORT_Alloc(len);
        if (!encodedAVA) {
            SECITEM_FreeItem(avaValue, PR_TRUE);
            if (unknownTag)
                PR_smprintf_free(unknownTag);
            return SECFailure;
        }
    } else {
        /* Must make output fit in tmpbuf */
        unsigned int fair = (sizeof tmpBuf) / 2 - 1; /* for = and \0 */

        if (nameLen < fair) {
            /* just truncate the value */
            maxValue = (sizeof tmpBuf) - (nameLen + 6); /* for "=...\0",
                                                     and possibly '"' */
        } else if (valueLen < fair) {
            /* just truncate the name */
            maxName = (sizeof tmpBuf) - (valueLen + 5); /* for "=...\0" */
        } else {
            /* truncate both */
            maxName = maxValue = fair - 3; /* for "..." */
        }
        if (nameLen > maxName) {
            PORT_Assert(unknownTag && unknownTag == tagName);
            truncateName = PR_TRUE;
            nameLen = maxName;
        }
        encodedAVA = tmpBuf;
    }

    memcpy(encodedAVA, tagName, nameLen);
    if (truncateName) {
        /* If tag name is too long, we know it is an OID form that was
     * allocated from the heap, so we can modify it in place
     */
        encodedAVA[nameLen - 1] = '.';
        encodedAVA[nameLen - 2] = '.';
        encodedAVA[nameLen - 3] = '.';
    }
    encodedAVA[nameLen++] = '=';
    if (unknownTag)
        PR_smprintf_free(unknownTag);

    if (strict == CERT_N2A_READABLE && maxValue > maxBytes)
        maxValue = maxBytes;
    if (valueLen > maxValue) {
        valueLen = maxValue;
        truncateValue = PR_TRUE;
    }
    /* escape and quote as necessary - don't quote hex strings */
    if (useHex) {
        char* end = encodedAVA + nameLen + valueLen;
        memcpy(encodedAVA + nameLen, (char*)avaValue->data, valueLen);
        end[0] = '\0';
        if (truncateValue) {
            end[-1] = '.';
            end[-2] = '.';
            end[-3] = '.';
        }
        rv = SECSuccess;
    } else if (!truncateValue) {
        rv = escapeAndQuote(encodedAVA + nameLen, len - nameLen,
                            (char*)avaValue->data, avaValue->len, &mode);
    } else {
        /* must truncate the escaped and quoted value */
        char bigTmpBuf[TMPBUF_LEN * 3 + 3];
        PORT_Assert(valueLen < sizeof tmpBuf);
        rv = escapeAndQuote(bigTmpBuf, sizeof bigTmpBuf, (char*)avaValue->data,
                            PR_MIN(avaValue->len, valueLen), &mode);

        bigTmpBuf[valueLen--] = '\0'; /* hard stop here */
        /* See if we're in the middle of a multi-byte UTF8 character */
        while (((bigTmpBuf[valueLen] & 0xc0) == 0x80) && valueLen > 0) {
            bigTmpBuf[valueLen--] = '\0';
        }
        /* add ellipsis to signify truncation. */
        bigTmpBuf[++valueLen] = '.';
        bigTmpBuf[++valueLen] = '.';
        bigTmpBuf[++valueLen] = '.';
        if (bigTmpBuf[0] == '"')
            bigTmpBuf[++valueLen] = '"';
        bigTmpBuf[++valueLen] = '\0';
        PORT_Assert(nameLen + valueLen <= (sizeof tmpBuf) - 1);
        memcpy(encodedAVA + nameLen, bigTmpBuf, valueLen + 1);
    }

    SECITEM_FreeItem(avaValue, PR_TRUE);
    if (rv == SECSuccess)
        rv = AppendStr(bufp, encodedAVA);
    if (encodedAVA != tmpBuf)
        PORT_Free(encodedAVA);
    return rv;
}

#undef tagName
#undef maxBytes
#undef tag
#undef vt

char*
CERT_NameToAsciiInvertible(CERTName* name, CertStrictnessLevel strict)
{
    CERTRDN** rdns;
    CERTRDN** lastRdn;
    CERTRDN** rdn;
    PRBool first = PR_TRUE;
    stringBuf strBuf = { NULL, 0, 0 };

    rdns = name->rdns;
    if (rdns == NULL) {
        return NULL;
    }

    /* find last RDN */
    lastRdn = rdns;
    while (*lastRdn)
        lastRdn++;
    lastRdn--;

    /*
   * Loop over name contents in _reverse_ RDN order appending to string
   */
    for (rdn = lastRdn; rdn >= rdns; rdn--) {
        CERTAVA** avas = (*rdn)->avas;
        CERTAVA* ava;
        PRBool newRDN = PR_TRUE;

        /*
     * XXX Do we need to traverse the AVAs in reverse order, too?
     */
        while (avas && (ava = *avas++) != NULL) {
            SECStatus rv;
            /* Put in comma or plus separator */
            if (!first) {
                /* Use of spaces is deprecated in RFC 2253. */
                rv = AppendStr(&strBuf, newRDN ? "," : "+");
                if (rv)
                    goto loser;
            } else {
                first = PR_FALSE;
            }

            /* Add in tag type plus value into strBuf */
            rv = AppendAVA(&strBuf, ava, strict);
            if (rv)
                goto loser;
            newRDN = PR_FALSE;
        }
    }
    return strBuf.buffer;
loser:
    if (strBuf.buffer) {
        PORT_Free(strBuf.buffer);
    }
    return NULL;
}

char*
CERT_NameToAscii(CERTName* name)
{
    return CERT_NameToAsciiInvertible(name, CERT_N2A_READABLE);
}

/*
 * Return the string representation of a DER encoded distinguished name
 * "dername" - The DER encoded name to convert
 */
char*
CERT_DerNameToAscii(SECItem* dername)
{
    int rv;
    PLArenaPool* arena = NULL;
    CERTName name;
    char* retstr = NULL;

    arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);

    if (arena == NULL) {
        goto loser;
    }

    rv = SEC_QuickDERDecodeItem(arena, &name, CERT_NameTemplate, dername);

    if (rv != SECSuccess) {
        goto loser;
    }

    retstr = CERT_NameToAscii(&name);

loser:
    if (arena != NULL) {
        PORT_FreeArena(arena, PR_FALSE);
    }

    return (retstr);
}

static char*
avaToString(PLArenaPool* arena, CERTAVA* ava)
{
    char* buf = NULL;
    SECItem* avaValue;
    int valueLen;

    avaValue = CERT_DecodeAVAValue(&ava->value);
    if (!avaValue) {
        return buf;
    }
    int reqLen = cert_RFC1485_GetRequiredLen((char*)avaValue->data, avaValue->len, NULL);
    /* reqLen is max 16384*3 + 2 */
    if (reqLen >= 0) {
        valueLen = reqLen + 1;
        if (arena) {
            buf = (char*)PORT_ArenaZAlloc(arena, valueLen);
        } else {
            buf = (char*)PORT_ZAlloc(valueLen);
        }
        if (buf) {
            SECStatus rv =
                escapeAndQuote(buf, valueLen, (char*)avaValue->data, avaValue->len, NULL);
            if (rv != SECSuccess) {
                if (!arena)
                    PORT_Free(buf);
                buf = NULL;
            }
        }
    }
    SECITEM_FreeItem(avaValue, PR_TRUE);
    return buf;
}

/* RDNs are sorted from most general to most specific.
 * This code returns the FIRST one found, the most general one found.
 */
static char*
CERT_GetNameElement(PLArenaPool* arena, const CERTName* name, int wantedTag)
{
    CERTRDN** rdns = name->rdns;
    CERTRDN* rdn;
    CERTAVA* ava = NULL;

    while (rdns && (rdn = *rdns++) != 0) {
        CERTAVA** avas = rdn->avas;
        while (avas && (ava = *avas++) != 0) {
            int tag = CERT_GetAVATag(ava);
            if (tag == wantedTag) {
                avas = NULL;
                rdns = NULL; /* break out of all loops */
            }
        }
    }
    return ava ? avaToString(arena, ava) : NULL;
}

/* RDNs are sorted from most general to most specific.
 * This code returns the LAST one found, the most specific one found.
 * This is particularly appropriate for Common Name.  See RFC 2818.
 */
static char*
CERT_GetLastNameElement(PLArenaPool* arena, const CERTName* name, int wantedTag)
{
    CERTRDN** rdns = name->rdns;
    CERTRDN* rdn;
    CERTAVA* lastAva = NULL;

    while (rdns && (rdn = *rdns++) != 0) {
        CERTAVA** avas = rdn->avas;
        CERTAVA* ava;
        while (avas && (ava = *avas++) != 0) {
            int tag = CERT_GetAVATag(ava);
            if (tag == wantedTag) {
                lastAva = ava;
            }
        }
    }
    return lastAva ? avaToString(arena, lastAva) : NULL;
}

char*
CERT_GetCertificateEmailAddress(CERTCertificate* cert)
{
    char* rawEmailAddr = NULL;
    SECItem subAltName;
    SECStatus rv;
    CERTGeneralName* nameList = NULL;
    CERTGeneralName* current;
    PLArenaPool* arena = NULL;
    int i;

    subAltName.data = NULL;

    rawEmailAddr = CERT_GetNameElement(cert->arena, &(cert->subject),
                                       SEC_OID_PKCS9_EMAIL_ADDRESS);
    if (rawEmailAddr == NULL) {
        rawEmailAddr =
            CERT_GetNameElement(cert->arena, &(cert->subject), SEC_OID_RFC1274_MAIL);
    }
    if (rawEmailAddr == NULL) {

        rv =
            CERT_FindCertExtension(cert, SEC_OID_X509_SUBJECT_ALT_NAME, &subAltName);
        if (rv != SECSuccess) {
            goto finish;
        }
        arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
        if (!arena) {
            goto finish;
        }
        nameList = current = CERT_DecodeAltNameExtension(arena, &subAltName);
        if (!nameList) {
            goto finish;
        }
        if (nameList != NULL) {
            do {
                if (current->type == certDirectoryName) {
                    rawEmailAddr =
                        CERT_GetNameElement(cert->arena, &(current->name.directoryName),
                                            SEC_OID_PKCS9_EMAIL_ADDRESS);
                    if (rawEmailAddr ==
                        NULL) {
                        rawEmailAddr =
                            CERT_GetNameElement(cert->arena, &(current->name.directoryName),
                                                SEC_OID_RFC1274_MAIL);
                    }
                } else if (current->type == certRFC822Name) {
                    rawEmailAddr =
                        (char*)PORT_ArenaZAlloc(cert->arena, current->name.other.len + 1);
                    if (!rawEmailAddr) {
                        goto finish;
                    }
                    PORT_Memcpy(rawEmailAddr, current->name.other.data,
                                current->name.other.len);
                    rawEmailAddr[current->name.other.len] =
                        '\0';
                }
                if (rawEmailAddr) {
                    break;
                }
                current = CERT_GetNextGeneralName(current);
            } while (current != nameList);
        }
    }
    if (rawEmailAddr) {
        for (i = 0; i <= (int)PORT_Strlen(rawEmailAddr); i++) {
            rawEmailAddr[i] = tolower(rawEmailAddr[i]);
        }
    }

finish:

    /* Don't free nameList, it's part of the arena. */

    if (arena) {
        PORT_FreeArena(arena, PR_FALSE);
    }

    if (subAltName.data) {
        SECITEM_FreeItem(&subAltName, PR_FALSE);
    }

    return (rawEmailAddr);
}

static char*
appendStringToBuf(char* dest, char* src, PRUint32* pRemaining)
{
    PRUint32 len;
    if (dest && src && src[0] && *pRemaining > (len = PL_strlen(src))) {
        PRUint32 i;
        for (i = 0; i < len; ++i)
            dest[i] = tolower(src[i]);
        dest[len] = 0;
        dest += len + 1;
        *pRemaining -= len + 1;
    }
    return dest;
}

#undef NEEDS_HEX_ESCAPE
#define NEEDS_HEX_ESCAPE(c) (c < 0x20)

static char*
appendItemToBuf(char* dest, SECItem* src, PRUint32* pRemaining)
{
    if (dest && src && src->data && src->len && src->data[0]) {
        PRUint32 len = src->len;
        PRUint32 i;
        PRUint32 reqLen = len + 1;
        /* are there any embedded control characters ? */
        for (i = 0; i < len; i++) {
            if (NEEDS_HEX_ESCAPE(src->data[i]))
                reqLen += 2;
        }
        if (*pRemaining > reqLen) {
            for (i = 0; i < len; ++i) {
                PRUint8 c = src->data[i];
                if (NEEDS_HEX_ESCAPE(c)) {
                    *dest++ =
                        C_BACKSLASH;
                    *dest++ =
                        hexChars[(c >> 4) & 0x0f];
                    *dest++ =
                        hexChars[c & 0x0f];
                } else {
                    *dest++ =
                        tolower(c);
                }
            }
            *dest++ = '\0';
            *pRemaining -= reqLen;
        }
    }
    return dest;
}

/* Returns a pointer to an environment-like string, a series of
** null-terminated strings, terminated by a zero-length string.
** This function is intended to be internal to NSS.
*/
char*
cert_GetCertificateEmailAddresses(CERTCertificate* cert)
{
    char* rawEmailAddr = NULL;
    char* addrBuf = NULL;
    char* pBuf = NULL;
    PORTCheapArenaPool tmpArena;
    PRUint32 maxLen = 0;
    PRInt32 finalLen = 0;
    SECStatus rv;
    SECItem subAltName;

    PORT_InitCheapArena(&tmpArena, DER_DEFAULT_CHUNKSIZE);

    subAltName.data = NULL;
    maxLen = cert->derCert.len;
    PORT_Assert(maxLen);
    if (!maxLen)
        maxLen = 2000; /* a guess, should never happen */

    pBuf = addrBuf = (char*)PORT_ArenaZAlloc(&tmpArena.arena, maxLen + 1);
    if (!addrBuf)
        goto loser;

    rawEmailAddr = CERT_GetNameElement(&tmpArena.arena, &cert->subject,
                                       SEC_OID_PKCS9_EMAIL_ADDRESS);
    pBuf = appendStringToBuf(pBuf, rawEmailAddr, &maxLen);

    rawEmailAddr = CERT_GetNameElement(&tmpArena.arena, &cert->subject,
                                       SEC_OID_RFC1274_MAIL);
    pBuf = appendStringToBuf(pBuf, rawEmailAddr, &maxLen);

    rv = CERT_FindCertExtension(cert, SEC_OID_X509_SUBJECT_ALT_NAME, &subAltName);
    if (rv == SECSuccess && subAltName.data) {
        CERTGeneralName* nameList = NULL;

        if (!!(nameList = CERT_DecodeAltNameExtension(&tmpArena.arena, &subAltName))) {
            CERTGeneralName* current = nameList;
            do {
                if (current->type == certDirectoryName) {
                    rawEmailAddr =
                        CERT_GetNameElement(&tmpArena.arena,
                                            &current->name.directoryName,
                                            SEC_OID_PKCS9_EMAIL_ADDRESS);
                    pBuf =
                        appendStringToBuf(pBuf, rawEmailAddr, &maxLen);

                    rawEmailAddr =
                        CERT_GetNameElement(&tmpArena.arena,
                                            &current->name.directoryName,
                                            SEC_OID_RFC1274_MAIL);
                    pBuf =
                        appendStringToBuf(pBuf, rawEmailAddr, &maxLen);
                } else if (current->type == certRFC822Name) {
                    pBuf =
                        appendItemToBuf(pBuf, &current->name.other, &maxLen);
                }
                current = CERT_GetNextGeneralName(current);
            } while (current != nameList);
        }
        SECITEM_FreeItem(&subAltName, PR_FALSE);
        /* Don't free nameList, it's part of the tmpArena. */
    }
    /* now copy superstring to cert's arena */
    finalLen = (pBuf - addrBuf) + 1;
    pBuf = NULL;
    if (finalLen > 1) {
        pBuf = PORT_ArenaAlloc(cert->arena, finalLen);
        if (pBuf) {
            PORT_Memcpy(pBuf, addrBuf, finalLen);
        }
    }
loser:
    PORT_DestroyCheapArena(&tmpArena);

    return pBuf;
}

/* returns pointer to storage in cert's arena.  Storage remains valid
** as long as cert's reference count doesn't go to zero.
** Caller should strdup or otherwise copy.
*/
const char* /* const so caller won't muck with it. */
CERT_GetFirstEmailAddress(CERTCertificate* cert)
{
    if (cert && cert->emailAddr && cert->emailAddr[0])
        return (const char*)cert->emailAddr;
    return NULL;
}

/* returns pointer to storage in cert's arena.  Storage remains valid
** as long as cert's reference count doesn't go to zero.
** Caller should strdup or otherwise copy.
*/
const char* /* const so caller won't muck with it. */
CERT_GetNextEmailAddress(CERTCertificate* cert, const char* prev)
{
    if (cert && prev && prev[0]) {
        PRUint32 len = PL_strlen(prev);
        prev += len + 1;
        if (prev && prev[0])
            return prev;
    }
    return NULL;
}

/* This is seriously bogus, now that certs store their email addresses in
** subject Alternative Name extensions.
** Returns a string allocated by PORT_StrDup, which the caller must free.
*/
char*
CERT_GetCertEmailAddress(const CERTName* name)
{
    char* rawEmailAddr;
    char* emailAddr;

    rawEmailAddr = CERT_GetNameElement(NULL, name, SEC_OID_PKCS9_EMAIL_ADDRESS);
    if (rawEmailAddr == NULL) {
        rawEmailAddr = CERT_GetNameElement(NULL, name, SEC_OID_RFC1274_MAIL);
    }
    emailAddr = CERT_FixupEmailAddr(rawEmailAddr);
    if (rawEmailAddr) {
        PORT_Free(rawEmailAddr);
    }
    return (emailAddr);
}

/* The return value must be freed with PORT_Free. */
char*
CERT_GetCommonName(const CERTName* name)
{
    return (CERT_GetLastNameElement(NULL, name, SEC_OID_AVA_COMMON_NAME));
}

char*
CERT_GetCountryName(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_AVA_COUNTRY_NAME));
}

char*
CERT_GetLocalityName(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_AVA_LOCALITY));
}

char*
CERT_GetStateName(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_AVA_STATE_OR_PROVINCE));
}

char*
CERT_GetOrgName(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_AVA_ORGANIZATION_NAME));
}

char*
CERT_GetDomainComponentName(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_AVA_DC));
}

char*
CERT_GetOrgUnitName(const CERTName* name)
{
    return (
        CERT_GetNameElement(NULL, name, SEC_OID_AVA_ORGANIZATIONAL_UNIT_NAME));
}

char*
CERT_GetDnQualifier(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_AVA_DN_QUALIFIER));
}

char*
CERT_GetCertUid(const CERTName* name)
{
    return (CERT_GetNameElement(NULL, name, SEC_OID_RFC1274_UID));
}
