/**
 * This macro is not present in Apache HTTP server version 2.2.3 
 * Red-Hat 5 / CentOS 5
 */
#ifndef ap_is_HTTP_VALID_RESPONSE
#  define ap_is_HTTP_VALID_RESPONSE(x) (((x) >= 100)&&((x) < 600))
#endif

/*
 * apr_time_from_msec is defined in APR 1.4.0 or later
 */
#if (APR_MAJOR_VERSION < 1) || ((APR_MAJOR_VERSION == 1) && (APR_MINOR_VERSION < 4))
#define apr_time_from_msec(x) (x * 1000)
#endif
