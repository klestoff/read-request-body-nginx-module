# Module ngx_http_read_request_body_module

The `ngx_http_read_request_body_module` module allows gain access to *$request_body* variable, even if not of `\*_pass` (`proxy_pass`, `fastcgi_pass`, etc.) directives is being used in current location

## Configuration example

```nginx
location / {
    read_request_body;
}
```

## Directives
<pre>
Syntax:  <b>read_request_body</b>;
Default: —
Context: server, location
</pre>

Read request body and made available *$request_body* variable.

_**Note**_: *$request_body* sets if only it's size less than **client_body_buffer_size**. For more information: [official documentation](http://nginx.org/en/docs/http/ngx_http_core_module.html#var_request_body).