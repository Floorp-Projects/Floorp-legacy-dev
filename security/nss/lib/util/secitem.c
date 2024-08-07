/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Support routines for SECItem data structure.
 */

#include "seccomon.h"
#include "secitem.h"
#include "secerr.h"
#include "secport.h"

SECItem *
SECITEM_AllocItem(PLArenaPool *arena, SECItem *item, unsigned int len)
{
    SECItem *result = NULL;
    void *mark = NULL;

    if (arena != NULL) {
        mark = PORT_ArenaMark(arena);
    }

    if (item == NULL) {
        if (arena != NULL) {
            result = PORT_ArenaZAlloc(arena, sizeof(SECItem));
        } else {
            result = PORT_ZAlloc(sizeof(SECItem));
        }
        if (result == NULL) {
            goto loser;
        }
    } else {
        PORT_Assert(item->data == NULL);
        result = item;
    }

    result->len = len;
    if (len) {
        if (arena != NULL) {
            result->data = PORT_ArenaAlloc(arena, len);
        } else {
            result->data = PORT_Alloc(len);
        }
        if (result->data == NULL) {
            goto loser;
        }
    } else {
        result->data = NULL;
    }

    if (mark) {
        PORT_ArenaUnmark(arena, mark);
    }
    return (result);

loser:
    if (arena != NULL) {
        if (mark) {
            PORT_ArenaRelease(arena, mark);
        }
        if (item != NULL) {
            item->data = NULL;
            item->len = 0;
        }
    } else {
        if (result != NULL) {
            SECITEM_FreeItem(result, (item == NULL) ? PR_TRUE : PR_FALSE);
        }
        /*
         * If item is not NULL, the above has set item->data and
         * item->len to 0.
         */
    }
    return (NULL);
}

SECStatus
SECITEM_MakeItem(PLArenaPool *arena, SECItem *dest, const unsigned char *data,
                 unsigned int len)
{
    SECItem it = { siBuffer, (unsigned char *)data, len };

    return SECITEM_CopyItem(arena, dest, &it);
}

SECStatus
SECITEM_ReallocItem(PLArenaPool *arena, SECItem *item, unsigned int oldlen,
                    unsigned int newlen)
{
    PORT_Assert(item != NULL);
    if (item == NULL) {
        /* XXX Set error.  But to what? */
        return SECFailure;
    }

    /*
     * If no old length, degenerate to just plain alloc.
     */
    if (oldlen == 0) {
        PORT_Assert(item->data == NULL || item->len == 0);
        if (newlen == 0) {
            /* Nothing to do.  Weird, but not a failure.  */
            return SECSuccess;
        }
        item->len = newlen;
        if (arena != NULL) {
            item->data = PORT_ArenaAlloc(arena, newlen);
        } else {
            item->data = PORT_Alloc(newlen);
        }
    } else {
        if (arena != NULL) {
            item->data = PORT_ArenaGrow(arena, item->data, oldlen, newlen);
        } else {
            item->data = PORT_Realloc(item->data, newlen);
        }
    }

    if (item->data == NULL) {
        return SECFailure;
    }

    return SECSuccess;
}

SECStatus
SECITEM_ReallocItemV2(PLArenaPool *arena, SECItem *item, unsigned int newlen)
{
    unsigned char *newdata = NULL;

    PORT_Assert(item);
    if (!item) {
        PORT_SetError(SEC_ERROR_INVALID_ARGS);
        return SECFailure;
    }

    if (item->len == newlen) {
        return SECSuccess;
    }

    if (!newlen) {
        if (!arena) {
            PORT_Free(item->data);
        }
        item->data = NULL;
        item->len = 0;
        return SECSuccess;
    }

    if (!item->data) {
        /* allocate fresh block of memory */
        PORT_Assert(!item->len);
        if (arena) {
            newdata = PORT_ArenaAlloc(arena, newlen);
        } else {
            newdata = PORT_Alloc(newlen);
        }
    } else {
        /* reallocate or adjust existing block of memory */
        if (arena) {
            if (item->len > newlen) {
                /* There's no need to realloc a shorter block from the arena,
                 * because it would result in using even more memory!
                 * Therefore we'll continue to use the old block and
                 * set the item to the shorter size.
                 */
                item->len = newlen;
                return SECSuccess;
            }
            newdata = PORT_ArenaGrow(arena, item->data, item->len, newlen);
        } else {
            newdata = PORT_Realloc(item->data, newlen);
        }
    }

    if (!newdata) {
        PORT_SetError(SEC_ERROR_NO_MEMORY);
        return SECFailure;
    }

    item->len = newlen;
    item->data = newdata;
    return SECSuccess;
}

SECComparison
SECITEM_CompareItem(const SECItem *a, const SECItem *b)
{
    unsigned m;
    int rv;

    if (a == b)
        return SECEqual;
    if (!a || !a->len || !a->data)
        return (!b || !b->len || !b->data) ? SECEqual : SECLessThan;
    if (!b || !b->len || !b->data)
        return SECGreaterThan;

    m = ((a->len < b->len) ? a->len : b->len);

    rv = PORT_Memcmp(a->data, b->data, m);
    if (rv) {
        return rv < 0 ? SECLessThan : SECGreaterThan;
    }
    if (a->len < b->len) {
        return SECLessThan;
    }
    if (a->len == b->len) {
        return SECEqual;
    }
    return SECGreaterThan;
}

PRBool
SECITEM_ItemsAreEqual(const SECItem *a, const SECItem *b)
{
    if (a->len != b->len)
        return PR_FALSE;
    if (!a->len)
        return PR_TRUE;
    if (!a->data || !b->data) {
        /* avoid null pointer crash. */
        return (PRBool)(a->data == b->data);
    }
    return (PRBool)!PORT_Memcmp(a->data, b->data, a->len);
}

SECItem *
SECITEM_DupItem(const SECItem *from)
{
    return SECITEM_ArenaDupItem(NULL, from);
}

SECItem *
SECITEM_ArenaDupItem(PLArenaPool *arena, const SECItem *from)
{
    SECItem *to;

    if (from == NULL) {
        return NULL;
    }

    to = SECITEM_AllocItem(arena, NULL, from->len);
    if (to == NULL) {
        return NULL;
    }

    to->type = from->type;
    if (to->len) {
        PORT_Memcpy(to->data, from->data, to->len);
    }

    return to;
}

SECStatus
SECITEM_CopyItem(PLArenaPool *arena, SECItem *to, const SECItem *from)
{
    to->type = from->type;
    if (from->data && from->len) {
        if (arena) {
            to->data = (unsigned char *)PORT_ArenaAlloc(arena, from->len);
        } else {
            to->data = (unsigned char *)PORT_Alloc(from->len);
        }

        if (!to->data) {
            return SECFailure;
        }
        PORT_Memcpy(to->data, from->data, from->len);
        to->len = from->len;
    } else {
        /*
         * If from->data is NULL but from->len is nonzero, this function
         * will succeed.  Is this right?
         */
        to->data = 0;
        to->len = 0;
    }
    return SECSuccess;
}

void
SECITEM_FreeItem(SECItem *zap, PRBool freeit)
{
    if (zap) {
        PORT_Free(zap->data);
        zap->data = 0;
        zap->len = 0;
        if (freeit) {
            PORT_Free(zap);
        }
    }
}

void
SECITEM_ZfreeItem(SECItem *zap, PRBool freeit)
{
    if (zap) {
        PORT_ZFree(zap->data, zap->len);
        zap->data = 0;
        zap->len = 0;
        if (freeit) {
            PORT_ZFree(zap, sizeof(SECItem));
        }
    }
}
/* these reroutines were taken from pkix oid.c, which is supposed to
 * replace this file some day */
/*
 * This is the hash function.  We simply XOR the encoded form with
 * itself in sizeof(PLHashNumber)-byte chunks.  Improving this
 * routine is left as an excercise for the more mathematically
 * inclined student.
 */
PLHashNumber PR_CALLBACK
SECITEM_Hash(const void *key)
{
    const SECItem *item = (const SECItem *)key;
    PLHashNumber rv = 0;

    PRUint8 *data = (PRUint8 *)item->data;
    PRUint32 i;
    PRUint8 *rvc = (PRUint8 *)&rv;

    for (i = 0; i < item->len; i++) {
        rvc[i % sizeof(rv)] ^= *data;
        data++;
    }

    return rv;
}

/*
 * This is the key-compare function.  It simply does a lexical
 * comparison on the item data.  This does not result in
 * quite the same ordering as the "sequence of numbers" order,
 * but heck it's only used internally by the hash table anyway.
 */
PRIntn PR_CALLBACK
SECITEM_HashCompare(const void *k1, const void *k2)
{
    const SECItem *i1 = (const SECItem *)k1;
    const SECItem *i2 = (const SECItem *)k2;

    return SECITEM_ItemsAreEqual(i1, i2);
}

SECItemArray *
SECITEM_AllocArray(PLArenaPool *arena, SECItemArray *array, unsigned int len)
{
    SECItemArray *result = NULL;
    void *mark = NULL;

    if (array != NULL && array->items != NULL) {
        PORT_Assert(0);
        PORT_SetError(SEC_ERROR_INVALID_ARGS);
        return NULL;
    }

    if (arena != NULL) {
        mark = PORT_ArenaMark(arena);
    }

    if (array == NULL) {
        if (arena != NULL) {
            result = PORT_ArenaZAlloc(arena, sizeof(SECItemArray));
        } else {
            result = PORT_ZAlloc(sizeof(SECItemArray));
        }
        if (result == NULL) {
            goto loser;
        }
    } else {
        result = array;
    }

    result->len = len;
    if (len) {
        if (arena != NULL) {
            result->items = PORT_ArenaZNewArray(arena, SECItem, len);
        } else {
            result->items = PORT_ZNewArray(SECItem, len);
        }
        if (result->items == NULL) {
            goto loser;
        }
    } else {
        result->items = NULL;
    }

    if (mark) {
        PORT_ArenaUnmark(arena, mark);
    }
    return result;

loser:
    if (arena != NULL) {
        if (mark) {
            PORT_ArenaRelease(arena, mark);
        }
    } else {
        if (result != NULL && array == NULL) {
            PORT_Free(result);
        }
    }
    if (array != NULL) {
        array->items = NULL;
        array->len = 0;
    }
    return NULL;
}

static void
secitem_FreeArray(SECItemArray *array, PRBool zero_items, PRBool freeit)
{
    unsigned int i;

    if (!array || !array->len || !array->items)
        return;

    for (i = 0; i < array->len; ++i) {
        SECItem *item = &array->items[i];

        if (item->data) {
            if (zero_items) {
                SECITEM_ZfreeItem(item, PR_FALSE);
            } else {
                SECITEM_FreeItem(item, PR_FALSE);
            }
        }
    }
    PORT_Free(array->items);
    array->items = NULL;
    array->len = 0;

    if (freeit)
        PORT_Free(array);
}

void
SECITEM_FreeArray(SECItemArray *array, PRBool freeit)
{
    secitem_FreeArray(array, PR_FALSE, freeit);
}

void
SECITEM_ZfreeArray(SECItemArray *array, PRBool freeit)
{
    secitem_FreeArray(array, PR_TRUE, freeit);
}

SECItemArray *
SECITEM_DupArray(PLArenaPool *arena, const SECItemArray *from)
{
    SECItemArray *result;
    unsigned int i;

    /* Require a "from" array.
     * Reject an inconsistent "from" array with NULL data and nonzero length.
     * However, allow a "from" array of zero length.
     */
    if (!from || (!from->items && from->len))
        return NULL;

    result = SECITEM_AllocArray(arena, NULL, from->len);
    if (!result)
        return NULL;

    for (i = 0; i < from->len; ++i) {
        SECStatus rv = SECITEM_CopyItem(arena,
                                        &result->items[i], &from->items[i]);
        if (rv != SECSuccess) {
            SECITEM_ZfreeArray(result, PR_TRUE);
            return NULL;
        }
    }

    return result;
}
