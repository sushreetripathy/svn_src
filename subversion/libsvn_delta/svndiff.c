/* 
 * svndiff.c -- Encoding and decoding svndiff-format deltas.
 * 
 * ================================================================
 * Copyright (c) 2000 Collab.Net.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by Collab.Net (http://www.Collab.Net/)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of Collab.Net.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLAB.NET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software may consist of voluntary contributions made by many
 * individuals on behalf of Collab.Net.
 */


#include <assert.h>
#include <string.h>
#include "svn_delta.h"
#include "svn_io.h"
#include "delta.h"

#define NORMAL_BITS 7
#define LENGTH_BITS 5


/* ----- Text delta to svndiff ----- */

/* We make one of these and get it passed back to us in calls to the
   window handler.  We only use it to record the write function and
   baton passed to svn_txdelta_to_svndiff ().  */
struct encoder_baton {
  svn_write_fn_t *write_fn;
  void *write_baton;
  apr_pool_t *pool;
};


/* Encode VAL into the buffer P using the variable-length svndiff
   integer format.  Return the incremented value of P after the
   encoded bytes have been written.

   BITS_IN_FIRST_BYTE should be 7 (NORMAL_BITS) except when we're
   encoding a length, in which case it's 5 (LENGTH_BITS) to leave room
   for the instruction selector.  Bytes after the first byte always
   use seven bits per byte.  Data bits go into the lowest-order bits
   of each byte, with the next-higher-order bit acting as a
   continuation bit.  High-order data bits are encoded first, followed
   by lower-order bits, so the value can be reconstructed by
   concatenating the data bits from left to right and interpreting the
   result as a binary number.

   Examples with BITS_IN_FIRST_BYTE being 7 (brackets denote byte
   boundaries, spaces are for clarity only):
           1 encodes as [0 0000001]
          33 encodes as [0 0100001]
         129 encodes as [1 0000001] [0 0000001]
        2000 encodes as [1 0001111] [0 1010000]

   Examples with BITS_IN_FIRST_BYTE being 5:
           1 encodes as [00 0 00001]
          33 encodes as [00 1 00000][0 0100001]
         129 encodes as [00 1 00001][0 0000001]
        2000 encodes as [00 1 01111][0 1010000] */

static char *
encode_int (unsigned char *p, apr_off_t val, int bits_in_first_byte)
{
  int n;
  apr_off_t v;
  unsigned char firstmask = (0x1 << bits_in_first_byte) - 1;
  unsigned char cont;

  assert (val >= 0);

  /* Figure out how many bytes we'll need after the first one.  */
  v = val >> bits_in_first_byte;
  n = 0;
  while (v > 0)
    {
      v = v >> 7;
      n++;
    }

  /* Encode the first byte.  */
  cont = ((n > 0) ? 0x1 : 0x0) << bits_in_first_byte;
  *p++ = ((val >> (n * 7)) & firstmask) | cont;

  /* Encode the remaining bytes; n is always the number of bytes
     coming after the one we're encoding.  */
  while (--n >= 0)
    {
      cont = ((n > 0) ? 0x1 : 0x0) << 7;
      *p++ = ((val >> (n * 7)) & 0x7f) | cont;
    }

  return p;
}


/* Append an integer encoded (using the normal number of bits in the
   first byte) to a string.  */
static void
append_encoded_int (svn_string_t *header, apr_off_t val, apr_pool_t *pool)
{
  char buf[128], *p;

  p = encode_int (buf, val, NORMAL_BITS);
  svn_string_appendbytes (header, buf, p - buf, pool);
}


static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  struct encoder_baton *eb = baton;
  apr_pool_t *pool = svn_pool_create (eb->pool, NULL);
  svn_string_t *instructions = svn_string_create ("", pool);
  svn_string_t *header = svn_string_create ("", pool);
  unsigned char ibuf[128], *ip;
  svn_txdelta_op_t *op;
  svn_error_t *err;
  apr_size_t len;

  if (window == NULL)
    {
      /* We're done; pass the word on to the output function and clean up.  */
      len = 0;
      err = eb->write_fn (eb->write_baton, NULL, &len, eb->pool);
      apr_destroy_pool (eb->pool);
      return SVN_NO_ERROR;
    }

  /* Encode the instructions.  */
  for (op = window->ops; op < window->ops + window->num_ops; op++)
    {
      /* Encode the action code and length.  */
      ip = encode_int (ibuf, op->length, LENGTH_BITS);
      switch (op->action_code)
        {
        case svn_txdelta_source: break;
        case svn_txdelta_target: *ibuf |= (0x1 << 6); break;
        case svn_txdelta_new:    *ibuf |= (0x2 << 6); break;
        }
      ip = encode_int (ip, op->offset, NORMAL_BITS);
      svn_string_appendbytes (instructions, ibuf, ip - ibuf, pool);
    }

  /* Encode the header.  */
  append_encoded_int (header, window->sview_offset, pool);
  append_encoded_int (header, window->sview_len, pool);
  append_encoded_int (header, window->tview_len, pool);
  append_encoded_int (header, instructions->len, pool);
  append_encoded_int (header, window->new->len, pool);

  /* Write out the window.  */
  len = header->len;
  err = eb->write_fn (eb->write_baton, header->data, &len, pool);
  if (err == SVN_NO_ERROR && instructions->len > 0)
    {
      len = instructions->len;
      err = eb->write_fn (eb->write_baton, instructions->data, &len, pool);
    }
  if (err == SVN_NO_ERROR && window->new->len > 0)
    {
      len = window->new->len;
      err = eb->write_fn (eb->write_baton, window->new->data, &len, pool);
    }

  apr_destroy_pool (pool);
  return err;
}

svn_error_t *
svn_txdelta_to_svndiff (svn_write_fn_t *write_fn,
			void *write_baton,
			apr_pool_t *pool,
			svn_txdelta_window_handler_t **handler,
			void **handler_baton)
{
  apr_pool_t *subpool = svn_pool_create (pool, NULL);
  apr_size_t len = 4;
  struct encoder_baton *eb;

  eb = apr_palloc (subpool, sizeof (*eb));
  eb->write_fn = write_fn;
  eb->write_baton = write_baton;
  eb->pool = subpool;

  *handler = window_handler;
  *handler_baton = eb;

  return write_fn(write_baton, "SVN\0", &len, subpool);
}



/* ----- svndiff to text delta ----- */

/* An svndiff parser object.  */
struct decode_baton
{
  /* Once the svndiff parser has enough data buffered to create a
     "window", it passes this window to the caller's consumer routine.  */
  svn_txdelta_window_handler_t *consumer_func;
  void *consumer_baton;

  /* Pool to create subpools from; each developing window will be a
     subpool.  */
  apr_pool_t *pool;

  /* The current subpool which contains our current window-buffer.  */
  apr_pool_t *subpool;

  /* The actual svndiff data buffer, living within subpool.  */
  svn_string_t *buffer;

  /* The offset and size of the last source view, so that we can check
     to make sure the next one isn't sliding backwards.  */
  apr_off_t last_sview_offset;
  apr_size_t last_sview_len;

  /* We have to discard four bytes at the beginning for the header.
     This field keeps track of how many of those bytes we have read.  */
  int header_bytes;
};


/* Decode an svndiff-encoded integer into VAL.  The bytes to be
   encoded live in the range [P..END-1].  BITS_IN_FIRST_BYTE should be
   7 (NORMAL_BITS) unless we're decoding a length, in which case it's
   5 (LENGTH_BITS) because the two high bits of the first length byte
   are taken up by the instruction selector.  See the comment for
   encode_int earlier in this file for more detail on the encoding
   format.  */

static const unsigned char *
decode_int (apr_off_t *val,
            const unsigned char *p,
            const unsigned char *end,
            int bits_in_first_byte)
{
  unsigned char firstmask = (0x1 << bits_in_first_byte) - 1;

  /* Decode the first byte.  */
  if (p == end)
    return NULL;
  *val = *p & firstmask;
  if (((*p++ >> bits_in_first_byte) & 0x1) == 0)
    return p;

  /* Decode bytes until we're done.  */
  while (p < end)
    {
      *val = (*val << 7) | (*p & 0x7f);
      if (((*p++ >> 7) & 0x1) == 0)
        return p;
    }
  return NULL;
}


static const unsigned char *
decode_instruction (svn_txdelta_op_t *op,
                    const unsigned char *p,
                    const unsigned char *end)
{
  apr_off_t val;

  if (p == end)
    return NULL;

  /* Decode the instruction selector.  */
  switch ((*p >> 6) & 0x3)
    {
    case 0x0: op->action_code = svn_txdelta_source; break;
    case 0x1: op->action_code = svn_txdelta_target; break;
    case 0x2: op->action_code = svn_txdelta_new; break;
    case 0x3: return NULL;
    }

  /* Decode the length and offset.  */
  p = decode_int (&val, p, end, LENGTH_BITS);
  if (p == NULL)
    return NULL;
  op->length = val;
  p = decode_int (&val, p, end, NORMAL_BITS);
  if (p == NULL)
    return NULL;
  op->offset = val;

  return p;
}

/* Count the instructions in the range [P..END-1] and make sure they
   are valid for the given window lengths.  Return -1 if the
   instructions are invalid; otherwise return the number of
   instructions.  */
static int
count_and_verify_instructions (const unsigned char *p,
                               const unsigned char *end,
                               apr_size_t sview_len,
                               apr_size_t tview_len,
                               apr_size_t new_len)
{
  int n = 0;
  svn_txdelta_op_t op;
  apr_size_t tpos = 0;

  while (p < end)
    {
      p = decode_instruction (&op, p, end);
      if (p == NULL || op.offset < 0 || op.length < 0
          || op.length > tview_len - tpos)
        return -1;
      switch (op.action_code)
        {
        case svn_txdelta_source:
          if (op.length > sview_len - op.offset)
            return -1;
          break;
        case svn_txdelta_target:
          if (op.offset >= tpos)
            return -1;
          break;
        case svn_txdelta_new:
          if (op.length > new_len - op.offset)
            return -1;
          break;
        }
      tpos += op.length;
      if (tpos < 0)
        return -1;
      n++;
    }
  if (tpos != tview_len)
    return -1;
  return n;
}

static svn_error_t *
write_handler (void *baton,
               const char *buffer,
               apr_size_t *len,
               apr_pool_t *pool)
{
  struct decode_baton *db = (struct decode_baton *) baton;
  const unsigned char *p, *end;
  apr_off_t val, sview_offset;
  apr_size_t sview_len, tview_len, inslen, newlen, remaining;
  svn_txdelta_window_t *window;
  svn_error_t *err;
  int ninst, i;

  if (*len == 0)
    {
      /* We're done.  Or we should be, anyway.  */
      /* XXX Check that db->buffer->len == 0 */
      if (db->header_bytes < 4 || db->buffer->len != 0)
        return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, pool,
                                 "unexpected end of svndiff input");
      db->consumer_func (NULL, db->consumer_baton);
      apr_destroy_pool (db->pool);
      return SVN_NO_ERROR;
    }

  /* Chew up four bytes at the beginning for the header.  */
  if (db->header_bytes < 4)
    {
      int nheader = 4 - db->header_bytes;
      if (nheader < *len)
        nheader = *len;
      if (memcmp (buffer, "SVN\0" + db->header_bytes, nheader) != 0)
        return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, pool,
                                 "svndiff has invalid header");
      *len -= nheader;
      db->header_bytes += nheader;
    }

  /* Concatenate the old with the new.  */
  svn_string_appendbytes (db->buffer, buffer, *len, db->subpool);

  /* Read the header, if we have enough bytes for that.  */
  p = (const unsigned char *) db->buffer->data;
  end = (const unsigned char *) db->buffer->data + db->buffer->len;

  p = decode_int (&val, p, end, 7);
  if (p == NULL)
    return SVN_NO_ERROR;
  sview_offset = val;

  p = decode_int (&val, p, end, 7);
  if (p == NULL)
    return SVN_NO_ERROR;
  sview_len = val;

  p = decode_int (&val, p, end, 7);
  if (p == NULL)
    return SVN_NO_ERROR;
  tview_len = val;


  p = decode_int (&val, p, end, 7);
  if (p == NULL)
    return SVN_NO_ERROR;
  inslen = val;

  p = decode_int (&val, p, end, 7);
  if (p == NULL)
    return SVN_NO_ERROR;
  newlen = val;

  /* Check for integer overflow (don't want to let the input trick us
     into invalid pointer games using negative numbers).  */
  if (sview_offset < 0 || sview_len < 0 || tview_len < 0 || inslen < 0
      || newlen < 0 || inslen + newlen < 0 || sview_offset + sview_len < 0)
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, pool,
                             "svndiff contains corrupt window header");

  /* Check for source windows which slide backwards.  */
  if (sview_offset < db->last_sview_offset
      || (sview_offset + sview_len
          < db->last_sview_offset + db->last_sview_len))
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, pool,
                             "svndiff has backwards-sliding source views");

  /* Now that we've read the header, we can determine the number of
     bytes in the rest of the window.  If we don't have that many,
     wait for more data.  */
  if (end - p < inslen + newlen)
    return SVN_NO_ERROR;

  /* Count the instructions and perform validity checks.  Return an
     error if there are invalid instructions, if there are any integer
     overflows, or if the source view slides backwards.  */
  end = p + inslen;
  ninst = count_and_verify_instructions (p, end, sview_len, tview_len, newlen);
  if (ninst == -1)
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, pool,
                             "svndiff contains invalid instructions");

  /* Build the window structure.  */
  window = apr_palloc (db->subpool, sizeof (*window));
  window->sview_offset = sview_offset;
  window->sview_len = sview_len;
  window->tview_len = tview_len;
  window->num_ops = ninst;
  window->ops_size = ninst;
  window->ops = apr_palloc (db->subpool, ninst * sizeof (*window->ops));
  for (i = 0; i < window->num_ops; i++)
    p = decode_instruction (&window->ops[i], p, end);
  window->new = svn_string_ncreate ((const char *) p, newlen, db->subpool);
  window->pool = db->subpool;

  /* Send it off.  */
  err = db->consumer_func (window, db->consumer_baton);

  /* Make a new subpool and buffer, saving aside the remaining data in
     the old buffer.  */
  db->subpool = svn_pool_create (db->pool, NULL);
  p += newlen;
  remaining = db->buffer->data + db->buffer->len - (const char *) p;
  db->buffer = svn_string_ncreate ((const char *) p, remaining, db->subpool);

  /* Remember the offset and length of the source view for next time.  */
  db->last_sview_offset = sview_offset;
  db->last_sview_len = sview_len;

  /* Free the window; this will also free up our old buffer.  */
  svn_txdelta_free_window (window);

  return err;
}

void window_compare (svn_txdelta_window_t *w1, svn_txdelta_window_t *w2)
{
  int i;

  assert (w1->sview_offset == w2->sview_offset);
  assert (w1->sview_len == w2->sview_len);
  assert (w1->tview_len == w2->tview_len);
  assert (w1->num_ops == w2->num_ops);
  for (i = 0; i < w1->num_ops; i++)
    {
      assert (w1->ops[i].action_code == w2->ops[i].action_code);
      assert (w1->ops[i].offset == w2->ops[i].offset);
      assert (w1->ops[i].length == w2->ops[i].length);
    }
  assert (w1->new->len == w2->new->len);
  assert (memcmp (w1->new->data, w2->new->data, w1->new->len) == 0);
}

svn_error_t *
svn_txdelta_parse_svndiff (svn_txdelta_window_handler_t *handler,
                           void *handler_baton,
                           apr_pool_t *pool,
                           svn_write_fn_t **write_fn,
                           void **write_baton)
{
  apr_pool_t *subpool = svn_pool_create (pool, NULL);
  struct decode_baton *db = apr_palloc (pool, sizeof (*db));

  db->consumer_func = handler;
  db->consumer_baton = handler_baton;
  db->pool = subpool;
  db->subpool = svn_pool_create (subpool, NULL);
  db->buffer = svn_string_create ("", db->subpool);
  db->last_sview_offset = 0;
  db->last_sview_len = 0;
  db->header_bytes = 0;
  *write_fn = write_handler;
  *write_baton = db;
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
