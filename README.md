# ngx_storm_module
this nginx's module for baofeng.com to using 
function ：
    read data from redis and replace the string for baofeng.com
### set nginx config file

    upstream storm_redis { 
        server  127.0.0.1:6379;
    }

    server {
        listen 80;
    
        location  / { 
            error_log logs/err_newcms.log  debug;
            storm_pass storm_redis;
        }
        error_page 404 @other;
    }
