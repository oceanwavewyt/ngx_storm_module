/**
 *@author wangyutian
 *storm protocol for baofeng.com
 *date 2017/5/10
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define STORM_KEY_LEN	   30
#define NGX_ESCAPE_STORM   4
#define NGX_HTTP_STORM_END   (sizeof(ngx_http_redis_end) - 1)
static u_char  ngx_http_redis_end[] = CRLF;                     
static ngx_str_t keyName = ngx_string("c");
static ngx_str_t content_type = ngx_string("text/html;charset=utf8");

typedef struct {
	u_char	*start;
	u_char 	*end; 
	u_char	*found_start;
	u_char  *found_end;
	u_char	*needmat_start;
	u_char	*needmat_end;
	size_t	pos_len;
	size_t  found_size;
} ngx_storm_pos_t;

static void *ngx_storm_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_storm_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_storm_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_storm_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_storm_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_storm_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_storm_filter_init(void *data);
static ngx_int_t ngx_http_storm_filter(void *data, ssize_t bytes);
static void ngx_http_storm_abort_request(ngx_http_request_t *r);
static void ngx_http_storm_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);
//static ngx_int_t ngx_http_bufdata_replace(ngx_http_request_t *r,ngx_chain_t *chain, ngx_str_t *match);
static void ngx_http_buf_find(ngx_http_request_t *r, u_char *pos, u_char *last, ngx_str_t *match, ngx_storm_pos_t *storm_pos);
static ngx_uint_t ngx_http_storm_url(ngx_http_request_t *r, ngx_str_t *argkey);


typedef struct {
    ngx_http_upstream_conf_t   upstream;
    ngx_int_t                  index;
    ngx_int_t                  db;
} ngx_http_storm_loc_conf_t;

typedef struct {
    size_t                     rest;
	size_t					   number;
    ngx_http_request_t        *request;
    ngx_str_t                  key;
	ngx_str_t				   argkey;
	ngx_str_t				   pid;
	ngx_str_t				   channel;
	ngx_str_t				   code;
	ngx_str_t				   mat_pid;
	ngx_str_t				   mat_channel;
	ngx_str_t				   mat_code;
	ngx_array_t				   *arr;
	ngx_array_t				   *mat_array;
	ngx_chain_t 			   *out;
	ngx_chain_t				   **last_out;
	u_char 					   *start;
	u_char					   *end;
	u_char					   storm_key[STORM_KEY_LEN];	
} ngx_http_storm_ctx_t;


static ngx_command_t ngx_storm_commands[] = {
	 	{ngx_string("storm_pass"),
		NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE1,
		ngx_http_storm_pass,
		NGX_HTTP_LOC_CONF_OFFSET,
		0,
		NULL},
	ngx_null_command
};

static ngx_http_module_t  ngx_http_storm_module_ctx = {
    NULL,                          /* preconfiguration */
    NULL,           /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */
    ngx_storm_create_loc_conf,  /* create location configuration */
    NULL /* merge location configuration */
};

ngx_module_t  ngx_storm_module = {
    NGX_MODULE_V1,
    &ngx_http_storm_module_ctx, /* module context */
    ngx_storm_commands,   /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *ngx_storm_create_loc_conf(ngx_conf_t *cf)
{
	ngx_http_storm_loc_conf_t  *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_storm_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

	conf->upstream.connect_timeout = 60000;
    conf->upstream.send_timeout = 60000;
    conf->upstream.read_timeout = 60000;

    conf->upstream.buffer_size = ngx_pagesize;

    conf->upstream.cyclic_temp_file = 0;
    conf->upstream.buffering = 0;
    conf->upstream.ignore_client_abort = 0;
    conf->upstream.send_lowat = 0;
    conf->upstream.bufs.num = 8;
    conf->upstream.bufs.size = ngx_pagesize;
	conf->upstream.busy_buffers_size = 2*ngx_pagesize;
    conf->upstream.max_temp_file_size = 1024*1024*1024;
    conf->upstream.temp_file_write_size = 2*ngx_pagesize;
    conf->upstream.intercept_errors = 1;
    conf->upstream.intercept_404 = 1;
    conf->upstream.pass_request_headers = 0;
    conf->upstream.pass_request_body = 0;

    conf->upstream.hide_headers = NGX_CONF_UNSET_PTR;
    conf->upstream.pass_headers = NGX_CONF_UNSET_PTR;

	return conf;
}


static ngx_int_t ngx_http_storm_handler(ngx_http_request_t *r)
{
	ngx_http_upstream_t *u;
	ngx_http_storm_loc_conf_t *rlcf;
	ngx_http_storm_ctx_t *ctx;	

	ngx_int_t rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }
	ngx_str_t argkey;
	if (NGX_OK != ngx_http_arg(r,keyName.data,keyName.len, &argkey)) {
		return NGX_ERROR;
	}

	if(ngx_http_upstream_create(r) != NGX_OK) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	u = r->upstream;
	ngx_str_set(&u->schema, "redis://");
	u->output.tag = (ngx_buf_tag_t) &ngx_storm_module;
	rlcf = ngx_http_get_module_loc_conf(r, ngx_storm_module);
	u->conf = &rlcf->upstream;

	ctx = ngx_palloc(r->pool, sizeof(ngx_http_storm_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->rest = NGX_HTTP_STORM_END;
    ctx->request = r;
	ctx->argkey = argkey;
	ctx->out = NULL;
    ctx->last_out = &ctx->out;
	ngx_http_set_ctx(r, ctx, ngx_storm_module);
	
	ngx_str_set(&ctx->pid, "");
	ngx_str_set(&ctx->channel, "");
	ngx_str_set(&ctx->code, "0");
	ngx_str_set(&ctx->mat_pid, "{$pid}");
	ngx_str_set(&ctx->mat_channel, "{$channel}");
	ngx_str_set(&ctx->mat_code, "{$ad_code}");

	//parser url and get storm key
	//ngx_unescape_uri
	ngx_str_t argkey_storm;
	argkey_storm.data = ngx_palloc(r->pool, sizeof(u_char)*argkey.len);		
	if (argkey_storm.data == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
	ngx_memcpy(argkey_storm.data, argkey.data,argkey.len);
	argkey_storm.len = argkey.len;
	ngx_http_storm_url(r, &argkey_storm);

	ctx->mat_array = ngx_array_create(ctx->request->pool,3,sizeof(ngx_str_t));
	ngx_str_t *mat = ngx_array_push(ctx->mat_array);
	mat->data = ctx->mat_pid.data;
	mat->len = ctx->mat_pid.len;
	mat = ngx_array_push(ctx->mat_array);
	mat->data = ctx->mat_channel.data;
	mat->len = ctx->mat_channel.len;
	mat = ngx_array_push(ctx->mat_array);
	mat->data = ctx->mat_code.data;
	mat->len = ctx->mat_code.len;


	ctx->arr = ngx_array_create(ctx->request->pool,3,sizeof(ngx_str_t));
	mat = ngx_array_push(ctx->arr);
	mat->data = ctx->pid.data;
	mat->len = ctx->pid.len;
	mat = ngx_array_push(ctx->arr);
	mat->data = ctx->channel.data;
	mat->len = ctx->channel.len;
	mat = ngx_array_push(ctx->arr);
	mat->data = ctx->code.data;
	mat->len = ctx->code.len;

	
	u->create_request = ngx_http_storm_create_request;
    u->reinit_request = ngx_http_storm_reinit_request;
    u->process_header = ngx_http_storm_process_header;
    u->abort_request = ngx_http_storm_abort_request;
    u->finalize_request = ngx_http_storm_finalize_request;
	u->input_filter_init = ngx_http_storm_filter_init;
    u->input_filter = ngx_http_storm_filter;
	u->input_filter_ctx = ctx;
	r->main->count++;
	ngx_http_upstream_init(r);
	return NGX_DONE;
} 


static char *ngx_http_storm_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_storm_loc_conf_t *rlcf = conf;
    ngx_str_t                 *value;
    ngx_url_t                  u;
    ngx_http_core_loc_conf_t  *clcf;

    if (rlcf->upstream.upstream) {
        return "is duplicate";
    }

    value = cf->args->elts;

    ngx_memzero(&u, sizeof(ngx_url_t));

    u.url = value[1];
    u.no_resolve = 1;

    rlcf->upstream.upstream = ngx_http_upstream_add(cf, &u, 0);
    if (rlcf->upstream.upstream == NULL) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_storm_handler;

    if (clcf->name.data[clcf->name.len - 1] == '/') {
        clcf->auto_redirect = 1;
    }

    //rlcf->index = ngx_http_get_variable_index(cf, &ngx_http_redis_key);

    //if (rlcf->index == NGX_ERROR) {
    //    return NGX_CONF_ERROR;
    //}
    return NGX_CONF_OK;
}


static ngx_int_t ngx_http_storm_create_request(ngx_http_request_t *r)
{
    size_t                          len;
    uintptr_t                       escape;
    ngx_buf_t                      *b;
    ngx_chain_t                    *cl;
    ngx_http_storm_ctx_t           *ctx;
    ngx_http_storm_loc_conf_t      *rlcf;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_storm_module);


    len = sizeof("select 0") - 1;
    len += sizeof(CRLF) - 1;
   	//ngx_str_t key = ngx_string("abc");
	ctx = ngx_http_get_module_ctx(r, ngx_storm_module);
	ngx_str_t key = ctx->argkey;
	/* Count have space required escape symbols. */
    escape = 2 * ngx_escape_uri(NULL, key.data, key.len, NGX_ESCAPE_STORM);

    len += sizeof("get ") - 1 + key.len + escape + sizeof(CRLF) - 1;

    /* Create temporary buffer for request with size len. */
    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    r->upstream->request_bufs = cl;
	/* Add "select " for request. */
    *b->last++ = 's'; *b->last++ = 'e'; *b->last++ = 'l'; *b->last++ = 'e';
    *b->last++ = 'c'; *b->last++ = 't'; *b->last++ = ' ';
    *b->last++ = '0';

    /* Add "\r\n". */
    *b->last++ = CR; *b->last++ = LF;
	/* Add "get" command with space. */
    *b->last++ = 'g'; *b->last++ = 'e'; *b->last++ = 't'; *b->last++ = ' ';

	ctx->key.data = b->last;
    //if (escape == 0) {
    //    b->last = ngx_copy(b->last, key.data, key.len);

    //} else {
        b->last = (u_char *) ngx_escape_uri(b->last, key.data, key.len,
                                           NGX_ESCAPE_STORM);
    //}

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http redis request: \"%V\"", &key);
	
	ctx->key.len = b->last - ctx->key.data;

    /* Add one more "\r\n". */
    *b->last++ = CR; *b->last++ = LF;
	
	
    /*select $redis_db\r\nget $redis_key\r\n" */
    return NGX_OK;
}

static ngx_int_t ngx_http_storm_reinit_request(ngx_http_request_t *r)
{
    return NGX_OK;
}


static ngx_int_t ngx_http_storm_process_header(ngx_http_request_t *r)
{
    u_char                    *p, *len;
    u_int                      c, try;
    ngx_str_t                  line;
    ngx_http_upstream_t       *u;
    //ngx_http_redis_ctx_t      *ctx;
    ngx_http_storm_loc_conf_t *rlcf;

    c = try = 0;

    u = r->upstream;

    p = u->buffer.pos;

    if (*p == '+') {
        try = 2;
    } else if (*p == '-') {
        try = 1;
    } else {
        goto no_valid;
    }

    for (p = u->buffer.pos; p < u->buffer.last; p++) {
        if (*p == LF) {
            c++;
            if (c == try) {
                goto found;
            }
        }
    }

    return NGX_AGAIN;

found:

    *p = '\0';

    line.len = p - u->buffer.pos - 1;
    line.data = u->buffer.pos;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "redis: \"%V\"", &line);

    p = u->buffer.pos;

    /* Get context of redis_key for future error messages, i.e. ctx->key */
    //ctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);
    rlcf = ngx_http_get_module_loc_conf(r, ngx_storm_module);

    /* Compare pointer and error message, if yes set 502 and return */
    if (ngx_strncmp(p, "-ERR", sizeof("-ERR") - 1) == 0) {
        //ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        //              "redis sent error in response \"%V\" "
        //              "for key \"%V\"",
        //              &line, &ctx->key);

        u->headers_in.status_n = 502;
        u->state->status = 502;

        return NGX_OK;
    }

    /* Compare pointer and good message, if yes move on the pointer */
    if (ngx_strncmp(p, "+OK\r\n", sizeof("+OK\r\n") - 1) == 0) {
        p += sizeof("+OK\r\n") - 1;
    }

    /*
 	 * Compare pointer and "get" answer.  As said before, "$" means, that
     * next symbols are length for upcoming key, "-1" means no key.
 	 * Set 404 and return.
 	**/
    if (ngx_strncmp(p, "$-1", sizeof("$-1") - 1) == 0) {
        //ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
        //              "key: \"%V\" was not found by redis", &ctx->key);

        u->headers_in.status_n = 404;
        u->state->status = 404;
        u->keepalive = 1;

        return NGX_OK;
    }

    /* Compare pointer and "get" answer, if "$"... */
    if (ngx_strncmp(p, "$", sizeof("$") - 1) == 0) {

        /* move on pointer */
        p += sizeof("$") - 1;

        /* set len to pointer */
        len = p;

        /* try to find end of string */
        while (*p && *p++ != CR) { /* void */ }

        /*
 *          * get the length of upcoming redis_key value, convert from ascii
 *                   * if the length is empty, return
 *                            */
#if defined nginx_version && nginx_version < 1001004
        r->headers_out.content_length_n = ngx_atoof(len, p - len - 1);
        if (r->headers_out.content_length_n == -1) {
#else
        u->headers_in.content_length_n = ngx_atoof(len, p - len - 1);
        if (u->headers_in.content_length_n == -1) {
#endif
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }
		r->headers_out.content_type = content_type; 
        /* The length of answer is not empty, set 200 */
        u->headers_in.status_n = 200;
        u->state->status = 200;
        /* Set position to the first symbol of data and return */
        u->buffer.pos = p + 1;

        return NGX_OK;
    }

no_valid:

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "redis sent invalid response: \"%V\"", &line);

    return NGX_HTTP_UPSTREAM_INVALID_HEADER;
}


static ngx_int_t ngx_http_storm_filter_init(void *data)
{
	ngx_http_storm_ctx_t  *ctx = data;

    ngx_http_upstream_t  *u;

    u = ctx->request->upstream;

#if defined nginx_version && nginx_version < 1005003
    u->length += NGX_HTTP_STORM_END;
#else
    if (u->headers_in.status_n != 404) {
        u->length = u->headers_in.content_length_n + NGX_HTTP_STORM_END;
        ctx->rest = NGX_HTTP_STORM_END;

    } else {
        u->length = 0;
    }
#endif

    return NGX_OK;
}


static ngx_int_t ngx_http_storm_filter(void *data, ssize_t bytes)
{
    ngx_http_storm_ctx_t  *ctx = data;

    u_char               *last, *curlast;
    ngx_buf_t            *b;
    ngx_chain_t          *cl, **ll;
    ngx_http_upstream_t  *u;

    u = ctx->request->upstream;
    b = &u->buffer;
		//ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
        //           "tiandiwang: %s", u->buffer.pos);
#if defined nginx_version && nginx_version < 1001004
    if (u->length == ctx->rest) {
#else
    if (u->length == (ssize_t) ctx->rest) {
#endif
        u->length -= bytes;
        ctx->rest -= bytes;

#if defined nginx_version && nginx_version >= 1001004
        if (u->length == 0) {
            u->keepalive = 1;
        }
#endif

        return NGX_OK;
    }
    for (cl = u->out_bufs, ll = &u->out_bufs; cl; cl = cl->next) {
        ll = &cl->next;
    }
	ctx->end = NULL;
	u_char *curpos;
	last = b->last;
	curpos = last;
	curlast = b->last+bytes;

	
	
	curpos = last;
	for(;;) {
		if(curpos >= curlast) break;
		ngx_uint_t i=0;
		ctx->number = 0;
		ngx_array_t *pos_list = ngx_array_create(ctx->request->pool, 3, sizeof(ngx_storm_pos_t));	
		if(pos_list == NULL){
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
		size_t min = 0;
		ngx_storm_pos_t *min_storm = NULL;
		for(;i<ctx->mat_array->nelts;i++) {
			ngx_str_t *mat =(ngx_str_t *)ctx->mat_array->elts+i;
			ngx_str_t *pmat =(ngx_str_t *) ctx->arr->elts+i;
			
			ngx_storm_pos_t *storm_pos = ngx_array_push(pos_list);
			ngx_http_buf_find(ctx->request,curpos, curlast, mat, storm_pos);		
			if(storm_pos->end < curlast) {
				storm_pos->found_start = storm_pos->end;
				storm_pos->found_end = storm_pos->end+mat->len;
				storm_pos->found_size = mat->len;
			}else{
				storm_pos->found_start = (void *)0;
				storm_pos->found_end = (void *)0;
				storm_pos->found_size = 0;
			}
			
			storm_pos->needmat_start = pmat->data;		
			storm_pos->needmat_end = pmat->data+pmat->len;
			storm_pos->pos_len = ctx->number;
			if(min == 0 || min > ctx->number) {
				 min = ctx->number;
				 min_storm = storm_pos;	 
			}
			ngx_log_debug4(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "wang_mat: %V,number:%d,min:%d,%s", mat,ctx->number,min,min_storm->needmat_start);
			ctx->number = 0; 
		}
		cl = ngx_chain_get_free_buf(ctx->request->pool, &u->free_bufs);
    	if (cl == NULL) {
        	return NGX_ERROR;
    	}
		if(min_storm->found_start) {
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "neeemat: %s", min_storm->found_start);
		}
    	cl->buf->flush = 1;
    	cl->buf->memory = 1;
		cl->next = NULL;
    	*ll = cl;
    	cl->buf->pos = min_storm->start;
    	cl->buf->last = min_storm->end;
    	cl->buf->tag = u->output.tag;
		ll = &cl->next;	
		//replace string
		if(min_storm->end < curlast) {	
			cl = ngx_chain_get_free_buf(ctx->request->pool, &u->free_bufs);
			if (cl == NULL) {
				return NGX_ERROR;
			}
			cl->buf->flush = 1;
			cl->buf->memory = 1;
			cl->buf->pos =  min_storm->needmat_start;       
			cl->buf->last = min_storm->needmat_end;
			cl->next = NULL; 
			*ll = cl;
			ll = &cl->next;
		}


		curpos = min_storm->end;		
		if(min_storm->end < curlast){
			curpos += min_storm->found_size;
		}
		ngx_array_destroy(pos_list);
	}
	
	b->last += bytes;
    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "redis filter bytes:%z size:%z length:%z rest:%z",
                   bytes, b->last - b->pos, u->length, ctx->rest);

    if (bytes <= (ssize_t) (u->length - NGX_HTTP_STORM_END)) {
        u->length -= bytes;
        return NGX_OK;
    }

    last += (size_t) (u->length - NGX_HTTP_STORM_END);

    if (ngx_strncmp(last, ngx_http_redis_end, b->last - last) != 0) {
        //ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
        //              "redis sent invalid trailer");
#if defined nginx_version && nginx_version >= 1001004
        b->last = last;
        cl->buf->last = last;
        u->length = 0;
        ctx->rest = 0;

        return NGX_OK;
#endif
    }

    ctx->rest -= b->last - last;
    b->last = last;
    cl->buf->last = last;
    u->length = ctx->rest;

#if defined nginx_version && nginx_version >= 1001004
        if (u->length == 0) {
            u->keepalive = 1;
        }
#endif
	
	/*	
	ngx_int_t rc = ngx_http_bufdata_replace(ctx->request, u->out_bufs, &mat);
	if(rc == NGX_ERROR){
		return rc;
	}    
	u->out_bufs = ctx->out;
*/
	return NGX_OK;
}


static void
ngx_http_storm_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort http redis request");
    return;
}


static void
ngx_http_storm_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http redis request");
    return;
}

static ngx_uint_t ngx_http_storm_url(ngx_http_request_t *r, ngx_str_t *argkey)
{
	u_char *src,*dest,*start,*last;
	src = argkey->data;
	dest = argkey->data;
	ngx_storm_pos_t storm_pos;
	ngx_str_t stormkey;
	ngx_http_storm_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_storm_module);
	//replace /7C/7C
	ngx_str_set(&stormkey, "/");	
	start = argkey->data;
	last = argkey->data + argkey->len;
	while(start<=last) {
		ctx->number = 0;	
		ngx_http_buf_find(r, start, last, &stormkey, &storm_pos);		
		if(storm_pos.end < last) {
			if(*(storm_pos.end+1) == '/') {
				start = storm_pos.end + 2;
				continue;
			}
			*storm_pos.end = '%';
		}
		start = storm_pos.end + stormkey.len;
	}
	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,"argkey: %s", argkey->data);
	ngx_unescape_uri(&dest,&src,argkey->len,0);
	*dest = '\0';
	argkey->len = ngx_strlen(argkey->data);
	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,"cparams: %s", argkey->data);
	last = argkey->data + argkey->len;
	
	ngx_str_set(&stormkey, "storm://");
	ngx_http_buf_find(r, argkey->data, last, &stormkey, &storm_pos); 			
	if(storm_pos.end > last || storm_pos.end+stormkey.len >=last) {
		return NGX_ERROR;
	}
	//find ||
	start = storm_pos.end+stormkey.len;
	ngx_str_set(&stormkey, "||");
	//find storm://131000023556
	ctx->number = 0;
	ngx_http_buf_find(r, start, last, &stormkey, &storm_pos);	
	ngx_memset(ctx->storm_key,0, STORM_KEY_LEN);
	ngx_snprintf(ctx->storm_key, STORM_KEY_LEN, "storm_");		
	ngx_memcpy(ctx->storm_key+6, start,ctx->number-stormkey.len);
	*(ctx->storm_key+ctx->number) = '\0';	
	ctx->argkey.data = ctx->storm_key;
	ctx->argkey.len = 6+ctx->number-stormkey.len;	
	ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,"argkey:%V, storm_key: %s,start:%s", &ctx->argkey, ctx->storm_key,start);
	ngx_str_t pid;
	ngx_str_set(&pid,"pid=");
	ngx_str_t channel;
	ngx_str_set(&channel,"channel=");

	start += ctx->number;
	while(start<=last){
		ctx->number = 0;
		ngx_http_buf_find(r, start, last, &stormkey, &storm_pos);
		size_t len = ctx->number;
		if(storm_pos.end < last) {
			len = ctx->number-stormkey.len;
		}
		if(ngx_strncasecmp(start, pid.data,pid.len)==0) {
			ctx->pid.data = start + pid.len;
			ctx->pid.len = len - pid.len;
		}else if(ngx_strncasecmp(start, channel.data,channel.len)==0) {
			ctx->channel.data = start + channel.len;
			ctx->channel.len = len - channel.len;
		}
		start += ctx->number;	
	}
	ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,"channels: %V,pids:%V", &ctx->channel, &ctx->pid);
	return NGX_OK;
}


static void ngx_http_buf_find(ngx_http_request_t *r, u_char *pos, u_char *last, ngx_str_t *match, ngx_storm_pos_t *storm_pos)
{
	u_char *p;
	u_char ch;
	ngx_uint_t state,looked;	
	state = 0;
	looked = 0;
	ngx_http_storm_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_storm_module);
	storm_pos->start = pos;
	for(p=pos;p<=last; p++){
		ctx->number++;	
		ch = ngx_tolower(*p);
		if(ch == match->data[looked]) {
			if(state == 0){
				storm_pos->end = p;
			}
			state = 1;	
			looked++;
			if(looked == match->len){
				//return storm_pos->start;	
				return;
			}	
		}else{
			looked = 0;	
			state = 0;		
			storm_pos->end = p;
		}	
	}
	storm_pos->end = last;
	//return storm_pos->start;	
	return;
}


