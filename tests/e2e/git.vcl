vcl 4.0;
import gitcache;

backend default { .host = "127.0.0.1"; .port = "9000"; }

sub vcl_init {
	new git = gitcache.cache(scope = "ghes1", purge_token = "e2e-purge");
}

sub vcl_recv {
	if (req.url ~ "^/_gitcache/purge") { return (synth(git.purge())); }
	if (git.recv()) { return (hash); }
}
sub vcl_hash             { if (git.hash()) { return (lookup); } }
sub vcl_backend_fetch    { git.backend_fetch(); }
sub vcl_backend_response { if (git.backend_response()) { return (deliver); } }
sub vcl_deliver          { if (git.deliver()) { return (restart); } }
