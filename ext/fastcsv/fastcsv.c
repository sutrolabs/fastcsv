
#line 1 "ext/fastcsv/fastcsv.rl"
#include <ruby.h>
#include <ruby/encoding.h>
// CSV specifications.
// http://tools.ietf.org/html/rfc4180
// http://w3c.github.io/csvw/syntax/#ebnf

// CSV implementation.
// https://github.com/ruby/ruby/blob/trunk/lib/csv.rb

// Ruby C extensions help.
// https://github.com/ruby/ruby/blob/trunk/README.EXT
// http://rxr.whitequark.org/mri/source

// Ragel help.
// https://www.mail-archive.com/ragel-users@complang.org/

#define ENCODE \
if (enc2 != NULL) { \
  field = rb_str_encode(field, rb_enc_from_encoding(enc), 0, Qnil); \
}

#define FREE \
if (buf != NULL) { \
  free(buf); \
}

static VALUE cClass, cParser, eError;
static ID s_read, s_row;

// @see https://github.com/nofxx/georuby_c/blob/b3b91fd90980d7c295ac8f6012d89878ea7cd569/ext/types.h#L22
typedef struct {
  char *start;
} Data;


#line 152 "ext/fastcsv/fastcsv.rl"



#line 43 "ext/fastcsv/fastcsv.c"
static const int raw_parse_start = 4;
static const int raw_parse_first_final = 4;
static const int raw_parse_error = 0;

static const int raw_parse_en_main = 4;


#line 155 "ext/fastcsv/fastcsv.rl"

// 16 kB
#define BUFSIZE 16384

// @see http://rxr.whitequark.org/mri/source/io.c#4845
static void rb_io_ext_int_to_encs(rb_encoding *ext, rb_encoding *intern, rb_encoding **enc, rb_encoding **enc2, int fmode) {
  int default_ext = 0;

  if (ext == NULL) {
    ext = rb_default_external_encoding();
    default_ext = 1;
  }
  if (ext == rb_ascii8bit_encoding()) {
    /* If external is ASCII-8BIT, no transcoding */
    intern = NULL;
  }
  else if (intern == NULL) {
    intern = rb_default_internal_encoding();
  }
  if (intern == NULL || intern == (rb_encoding *)Qnil || intern == ext) {
    /* No internal encoding => use external + no transcoding */
    *enc = (default_ext && intern != ext) ? NULL : ext;
    *enc2 = NULL;
  }
  else {
    *enc = intern;
    *enc2 = ext;
  }
}

static VALUE raw_parse(int argc, VALUE *argv, VALUE self) {
  int cs, act, have = 0, curline = 1, io = 0;
  char *ts = 0, *te = 0, *buf = 0, *eof = 0;

  VALUE port, opts, r_encoding;
  VALUE row = rb_ary_new(), field = Qnil, bufsize = Qnil;
  int done = 0, unclosed_line = 0, buffer_size = 0, taint = 0;
  rb_encoding *enc = NULL, *enc2 = NULL, *encoding = NULL;

  Data *d;
  Data_Get_Struct(self, Data, d);

  VALUE option;
  char quote_char = '"';

  rb_scan_args(argc, argv, "11", &port, &opts);
  taint = OBJ_TAINTED(port);
  io = rb_respond_to(port, s_read);
  if (!io) {
    if (rb_respond_to(port, rb_intern("to_str"))) {
      port = rb_funcall(port, rb_intern("to_str"), 0);
      StringValue(port);
    }
    else {
      rb_raise(rb_eArgError, "data has to respond to #read or #to_str");
    }
  }

  if (NIL_P(opts)) {
    opts = rb_hash_new();
  }
  else if (TYPE(opts) != T_HASH) {
    rb_raise(rb_eArgError, "options has to be a Hash or nil");
  }

  // @see rb_io_extract_modeenc
  /* Set to defaults */
  rb_io_ext_int_to_encs(NULL, NULL, &enc, &enc2, 0);

  // "enc" (internal) or "enc2:enc" (external:internal) or "enc:-" (external).
  // We don't support binmode, which would force "ASCII-8BIT", or "BOM|UTF-*".
  // @see http://ruby-doc.org/core-2.1.1/IO.html#method-c-new-label-Open+Mode
  option = rb_hash_aref(opts, ID2SYM(rb_intern("encoding")));
  if (TYPE(option) == T_STRING) {
    // `parse_mode_enc` is not in header file.
    const char *estr = StringValueCStr(option), *ptr;
    char encname[ENCODING_MAXNAMELEN+1];
    int idx, idx2;
    rb_encoding *ext_enc, *int_enc;

    /* parse estr as "enc" or "enc2:enc" or "enc:-" */

    ptr = strrchr(estr, ':');
    if (ptr) {
      long len = (ptr++) - estr;
      if (len == 0 || len > ENCODING_MAXNAMELEN) { // ":enc"
        idx = -1;
      }
      else { // "enc2:enc" or "enc:-"
        memcpy(encname, estr, len);
        encname[len] = '\0';
        estr = encname;
        idx = rb_enc_find_index(encname);
      }
    }
    else { // "enc"
      idx = rb_enc_find_index(estr);
    }

    if (idx >= 0) {
      ext_enc = rb_enc_from_index(idx);
    }
    else {
      if (idx != -2) { // ":enc"
        // `unsupported_encoding` is not in header file.
        rb_warn("Unsupported encoding %s ignored", estr);
      }
      ext_enc = NULL;
    }

    int_enc = NULL;
    if (ptr) {
      if (*ptr == '-' && *(ptr+1) == '\0') { // "enc:-"
        /* Special case - "-" => no transcoding */
        int_enc = (rb_encoding *)Qnil;
      }
      else { // "enc2:enc"
        idx2 = rb_enc_find_index(ptr);
        if (idx2 < 0) {
          // `unsupported_encoding` is not in header file.
          rb_warn("Unsupported encoding %s ignored", ptr);
        }
        else if (idx2 == idx) {
          int_enc = (rb_encoding *)Qnil;
        }
        else {
          int_enc = rb_enc_from_index(idx2);
        }
      }
    }

    rb_io_ext_int_to_encs(ext_enc, int_enc, &enc, &enc2, 0);
  }
  else if (!NIL_P(option)) {
    rb_raise(rb_eArgError, ":encoding has to be a String");
  }

  // @see CSV#raw_encoding
  // @see https://github.com/ruby/ruby/blob/ab337e61ecb5f42384ba7d710c36faf96a454e5c/lib/csv.rb#L2290
  if (rb_respond_to(port, rb_intern("internal_encoding"))) {
    r_encoding = rb_funcall(port, rb_intern("internal_encoding"), 0);
    if (NIL_P(r_encoding)) {
      r_encoding = rb_funcall(port, rb_intern("external_encoding"), 0);
    }
  }
  else if (rb_respond_to(port, rb_intern("string"))) {
    r_encoding = rb_funcall(rb_funcall(port, rb_intern("string"), 0), rb_intern("encoding"), 0);
  }
  else if (rb_respond_to(port, rb_intern("encoding"))) {
    r_encoding = rb_funcall(port, rb_intern("encoding"), 0);
  }
  else {
    r_encoding = rb_enc_from_encoding(rb_ascii8bit_encoding());
  }

  // @see CSV#initialize
  // @see https://github.com/ruby/ruby/blob/ab337e61ecb5f42384ba7d710c36faf96a454e5c/lib/csv.rb#L1510
  if (NIL_P(r_encoding)) {
    r_encoding = rb_enc_from_encoding(rb_default_internal_encoding());
  }
  if (NIL_P(r_encoding)) {
    r_encoding = rb_enc_from_encoding(rb_default_external_encoding());
  }

  if (enc2 != NULL) {
    encoding = enc2;
  }
  else if (enc != NULL) {
    encoding = enc;
  }
  else if (!NIL_P(r_encoding)) {
    encoding = rb_enc_get(r_encoding);
  }

  // In case #raw_parse is called multiple times on the same parser. Note that
  // using IO methods on a re-used parser can cause segmentation faults.
  rb_ivar_set(self, s_row, Qnil);

  buffer_size = BUFSIZE;
  if (rb_ivar_defined(self, rb_intern("@buffer_size")) == Qtrue) {
    bufsize = rb_ivar_get(self, rb_intern("@buffer_size"));
    if (!NIL_P(bufsize)) {
      buffer_size = NUM2INT(bufsize);
      // buffer_size = 0 can cause segmentation faults.
      if (buffer_size == 0) {
        buffer_size = BUFSIZE;
      }
    }
  }

  if (io) {
    buf = ALLOC_N(char, buffer_size);
  }

  
#line 247 "ext/fastcsv/fastcsv.c"
	{
	cs = raw_parse_start;
	ts = 0;
	te = 0;
	act = 0;
	}

#line 350 "ext/fastcsv/fastcsv.rl"

  while (!done) {
    VALUE str;
    char *p, *pe;
    int len, space = buffer_size - have, tokstart_diff, tokend_diff, start_diff;

    if (io) {
      if (space == 0) {
        // Not moving d->start will cause intermittent segmentation faults.
        tokstart_diff = ts - buf;
        tokend_diff = te - buf;
        start_diff = d->start - buf;

        buffer_size += BUFSIZE;
        REALLOC_N(buf, char, buffer_size);

        space = buffer_size - have;

        ts = buf + tokstart_diff;
        te = buf + tokend_diff;
        d->start = buf + start_diff;
      }
      p = buf + have;

      // Reads "`length` bytes without any conversion (binary mode)."
      // "The resulted string is always ASCII-8BIT encoding."
      // @see http://www.ruby-doc.org/core-2.1.4/IO.html#method-i-read
      str = rb_funcall(port, s_read, 1, INT2FIX(space));
      if (NIL_P(str)) {
        // "`nil` means it met EOF at beginning," e.g. for `StringIO.new("")`.
        len = 0;
      }
      else {
        len = RSTRING_LEN(str);
        memcpy(p, StringValuePtr(str), len);
      }

      // "The 1 to `length`-1 bytes string means it met EOF after reading the result."
      if (len < space) {
        // EOF actions don't work in scanners, so we add a sentinel value.
        // @see http://www.complang.org/pipermail/ragel-users/2007-May/001516.html
        // @see https://github.com/leeonix/lua-csv-ragel/blob/master/src/csv.rl
        p[len++] = 0;
        done = 1;
      }
    }
    else {
      p = RSTRING_PTR(port);
      len = RSTRING_LEN(port);
      p[len++] = 0;
      done = 1;
    }

    if (d->start == 0) {
      d->start = p;
    }

    pe = p + len;
    
#line 315 "ext/fastcsv/fastcsv.c"
	{
	if ( p == pe )
		goto _test_eof;
	switch ( cs )
	{
tr0:
#line 1 "NONE"
	{	switch( act ) {
	case 0:
	{{goto st0;}}
	break;
	default:
	{{p = ((te))-1;}}
	break;
	}
	}
	goto st4;
tr5:
#line 46 "ext/fastcsv/fastcsv.rl"
	{
    if (p == ts) {
      // Unquoted empty fields are nil, not "", in Ruby.
      field = Qnil;
    }
    else if (p > ts) {
      field = rb_enc_str_new(ts, p - ts, encoding);
      ENCODE;
    }
  }
#line 92 "ext/fastcsv/fastcsv.rl"
	{
    rb_ary_push(row, field);
    field = Qnil;
  }
#line 148 "ext/fastcsv/fastcsv.rl"
	{te = p+1;}
	goto st4;
tr9:
#line 120 "ext/fastcsv/fastcsv.rl"
	{
    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) {
      rb_ary_push(row, field);
    }

    if (RARRAY_LEN(row)) {
      rb_yield(row);
    }
  }
#line 150 "ext/fastcsv/fastcsv.rl"
	{te = p+1;}
	goto st4;
tr12:
#line 92 "ext/fastcsv/fastcsv.rl"
	{
    rb_ary_push(row, field);
    field = Qnil;
  }
#line 148 "ext/fastcsv/fastcsv.rl"
	{te = p+1;}
	goto st4;
tr15:
#line 150 "ext/fastcsv/fastcsv.rl"
	{te = p;p--;}
	goto st4;
tr16:
#line 97 "ext/fastcsv/fastcsv.rl"
	{
    d->start = p;
  }
#line 149 "ext/fastcsv/fastcsv.rl"
	{te = p;p--;}
	goto st4;
st4:
#line 1 "NONE"
	{ts = 0;}
#line 1 "NONE"
	{act = 0;}
	if ( ++p == pe )
		goto _test_eof4;
case 4:
#line 1 "NONE"
	{ts = p;}
#line 405 "ext/fastcsv/fastcsv.c"
	switch( (*p) ) {
		case 0: goto tr13;
		case 10: goto tr3;
		case 13: goto tr4;
		case 34: goto tr14;
		case 44: goto tr5;
	}
	goto st1;
st1:
	if ( ++p == pe )
		goto _test_eof1;
case 1:
	switch( (*p) ) {
		case 0: goto tr2;
		case 10: goto tr3;
		case 13: goto tr4;
		case 34: goto tr0;
		case 44: goto tr5;
	}
	goto st1;
tr2:
#line 1 "NONE"
	{te = p+1;}
#line 46 "ext/fastcsv/fastcsv.rl"
	{
    if (p == ts) {
      // Unquoted empty fields are nil, not "", in Ruby.
      field = Qnil;
    }
    else if (p > ts) {
      field = rb_enc_str_new(ts, p - ts, encoding);
      ENCODE;
    }
  }
#line 120 "ext/fastcsv/fastcsv.rl"
	{
    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) {
      rb_ary_push(row, field);
    }

    if (RARRAY_LEN(row)) {
      rb_yield(row);
    }
  }
#line 150 "ext/fastcsv/fastcsv.rl"
	{act = 3;}
	goto st5;
st5:
	if ( ++p == pe )
		goto _test_eof5;
case 5:
#line 464 "ext/fastcsv/fastcsv.c"
	switch( (*p) ) {
		case 0: goto tr2;
		case 10: goto tr3;
		case 13: goto tr4;
		case 34: goto tr15;
		case 44: goto tr5;
	}
	goto st1;
tr3:
#line 46 "ext/fastcsv/fastcsv.rl"
	{
    if (p == ts) {
      // Unquoted empty fields are nil, not "", in Ruby.
      field = Qnil;
    }
    else if (p > ts) {
      field = rb_enc_str_new(ts, p - ts, encoding);
      ENCODE;
    }
  }
#line 101 "ext/fastcsv/fastcsv.rl"
	{
    curline++;

    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) { // same as new_field
      rb_ary_push(row, field);
      field = Qnil;
    }

    rb_yield(row);
    row = rb_ary_new();
  }
	goto st6;
tr10:
#line 101 "ext/fastcsv/fastcsv.rl"
	{
    curline++;

    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) { // same as new_field
      rb_ary_push(row, field);
      field = Qnil;
    }

    rb_yield(row);
    row = rb_ary_new();
  }
	goto st6;
st6:
	if ( ++p == pe )
		goto _test_eof6;
case 6:
#line 530 "ext/fastcsv/fastcsv.c"
	goto tr16;
tr4:
#line 46 "ext/fastcsv/fastcsv.rl"
	{
    if (p == ts) {
      // Unquoted empty fields are nil, not "", in Ruby.
      field = Qnil;
    }
    else if (p > ts) {
      field = rb_enc_str_new(ts, p - ts, encoding);
      ENCODE;
    }
  }
#line 101 "ext/fastcsv/fastcsv.rl"
	{
    curline++;

    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) { // same as new_field
      rb_ary_push(row, field);
      field = Qnil;
    }

    rb_yield(row);
    row = rb_ary_new();
  }
	goto st7;
tr11:
#line 101 "ext/fastcsv/fastcsv.rl"
	{
    curline++;

    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) { // same as new_field
      rb_ary_push(row, field);
      field = Qnil;
    }

    rb_yield(row);
    row = rb_ary_new();
  }
	goto st7;
st7:
	if ( ++p == pe )
		goto _test_eof7;
case 7:
#line 589 "ext/fastcsv/fastcsv.c"
	if ( (*p) == 10 )
		goto st6;
	goto tr16;
tr13:
#line 1 "NONE"
	{te = p+1;}
#line 46 "ext/fastcsv/fastcsv.rl"
	{
    if (p == ts) {
      // Unquoted empty fields are nil, not "", in Ruby.
      field = Qnil;
    }
    else if (p > ts) {
      field = rb_enc_str_new(ts, p - ts, encoding);
      ENCODE;
    }
  }
#line 120 "ext/fastcsv/fastcsv.rl"
	{
    if (d->start == 0 || p == d->start) {
      rb_ivar_set(self, s_row, rb_str_new2(""));
    }
    else if (p > d->start) {
      rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
    }

    if (!NIL_P(field) || RARRAY_LEN(row)) {
      rb_ary_push(row, field);
    }

    if (RARRAY_LEN(row)) {
      rb_yield(row);
    }
  }
#line 150 "ext/fastcsv/fastcsv.rl"
	{act = 3;}
	goto st8;
st8:
	if ( ++p == pe )
		goto _test_eof8;
case 8:
#line 631 "ext/fastcsv/fastcsv.c"
	switch( (*p) ) {
		case 10: goto tr15;
		case 13: goto tr15;
		case 34: goto tr15;
		case 44: goto tr15;
	}
	goto st1;
tr14:
#line 38 "ext/fastcsv/fastcsv.rl"
	{
    unclosed_line = curline;
  }
	goto st2;
st2:
	if ( ++p == pe )
		goto _test_eof2;
case 2:
#line 649 "ext/fastcsv/fastcsv.c"
	switch( (*p) ) {
		case 0: goto st0;
		case 34: goto tr8;
	}
	goto st2;
st0:
cs = 0;
	goto _out;
tr8:
#line 57 "ext/fastcsv/fastcsv.rl"
	{
    if (p == ts) {
      field = rb_enc_str_new("", 0, encoding);
      ENCODE;
    }
    // @note If we add an action on '""', we can skip some steps if no '""' is found.
    else if (p > ts) {
      // Operating on ts in-place produces odd behavior, FYI.
      char *copy = ALLOC_N(char, p - ts);
      memcpy(copy, ts, p - ts);

      char *reader = ts, *writer = copy;
      int escaped = 0;

      while (p > reader) {
        if (*reader == quote_char && !escaped) {
          // Skip the escaping character.
          escaped = 1;
        }
        else {
          escaped = 0;
          *writer++ = *reader;
        }
        reader++;
      }

      field = rb_enc_str_new(copy, writer - copy, encoding);
      ENCODE;

      if (copy != NULL) {
        free(copy);
      }
    }
  }
#line 42 "ext/fastcsv/fastcsv.rl"
	{
    unclosed_line = 0;
  }
	goto st3;
st3:
	if ( ++p == pe )
		goto _test_eof3;
case 3:
#line 703 "ext/fastcsv/fastcsv.c"
	switch( (*p) ) {
		case 0: goto tr9;
		case 10: goto tr10;
		case 13: goto tr11;
		case 34: goto st2;
		case 44: goto tr12;
	}
	goto st0;
	}
	_test_eof4: cs = 4; goto _test_eof; 
	_test_eof1: cs = 1; goto _test_eof; 
	_test_eof5: cs = 5; goto _test_eof; 
	_test_eof6: cs = 6; goto _test_eof; 
	_test_eof7: cs = 7; goto _test_eof; 
	_test_eof8: cs = 8; goto _test_eof; 
	_test_eof2: cs = 2; goto _test_eof; 
	_test_eof3: cs = 3; goto _test_eof; 

	_test_eof: {}
	if ( p == eof )
	{
	switch ( cs ) {
	case 1: goto tr0;
	case 5: goto tr15;
	case 6: goto tr16;
	case 7: goto tr16;
	case 8: goto tr15;
	}
	}

	_out: {}
	}

#line 409 "ext/fastcsv/fastcsv.rl"

    if (done && cs < raw_parse_first_final) {
      if (d->start == 0 || p == d->start) {
        rb_ivar_set(self, s_row, rb_str_new2(""));
      }
      else if (p > d->start) {
        rb_ivar_set(self, s_row, rb_str_new(d->start, p - d->start));
      }

      FREE;

      if (unclosed_line) {
        rb_raise(eError, "Unclosed quoted field on line %d.", unclosed_line);
      }
      else {
        rb_raise(eError, "Illegal quoting in line %d.", curline);
      }
    }

    if (ts == 0) {
      have = 0;
    }
    else if (io) {
      have = pe - ts;
      memmove(buf, ts, have);
      te = buf + (te - ts);
      ts = buf;
    }
  }

  FREE;

  return Qnil;
}

// @see https://github.com/ruby/ruby/blob/trunk/README.EXT#L616
static VALUE allocate(VALUE class) {
  // @see https://github.com/nofxx/georuby_c/blob/b3b91fd90980d7c295ac8f6012d89878ea7cd569/ext/line.c#L66
  Data *d = ALLOC(Data);
  d->start = 0;
  // @see https://github.com/nofxx/georuby_c/blob/b3b91fd90980d7c295ac8f6012d89878ea7cd569/ext/point.h#L26
  // rb_gc_mark(d->start) or rb_gc_mark(d) cause warning "passing argument 1 of ‘rb_gc_mark’ makes integer from pointer without a cast"
  // free(d->start) causes error "pointer being freed was not allocated"
  return Data_Wrap_Struct(class, NULL, free, d);
}

// @see http://tenderlovemaking.com/2009/12/18/writing-ruby-c-extensions-part-1.html
// @see http://tenderlovemaking.com/2010/12/11/writing-ruby-c-extensions-part-2.html
void Init_fastcsv() {
  s_read = rb_intern("read");
  s_row = rb_intern("@row");

  cClass = rb_define_class("FastCSV", rb_const_get(rb_cObject, rb_intern("CSV"))); // class FastCSV < CSV
  cParser = rb_define_class_under(cClass, "Parser", rb_cObject);                   //   class Parser
  rb_define_alloc_func(cParser, allocate);                                         //
  rb_define_method(cParser, "raw_parse", raw_parse, -1);                           //     def raw_parse(port, opts = nil); end
  rb_define_attr(cParser, "row", 1, 0);                                            //     attr_reader :row
  rb_define_attr(cParser, "buffer_size", 1, 1);                                    //     attr_accessor :buffer_size
                                                                                   //   end
  eError = rb_define_class_under(cClass, "MalformedCSVError", rb_eRuntimeError);   //   class MalformedCSVError < RuntimeError
                                                                                   // end
}
