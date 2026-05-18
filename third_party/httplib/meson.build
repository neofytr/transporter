# httplib gates all OpenSSL code behind `#ifdef CPPHTTPLIB_OPENSSL_SUPPORT`,
# which tests definedness, not value: passing -D...=0 still pulls in libssl
# and breaks the link. Leave the macro undefined to keep TLS out entirely.
httplib_dep = declare_dependency(
  include_directories : include_directories('.'),
)
