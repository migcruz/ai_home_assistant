#pragma once

/*
 * Pinned TLS certificate for vulcan (CN=vulcan.local, self-signed, RSA 2048).
 * Valid 2026-03-06 → 2036-03-03.
 *
 * To regenerate after renewing the nginx certificate:
 *   echo | openssl s_client -connect localhost:443 2>/dev/null \
 *     | openssl x509 -outform PEM
 * Then replace the base64 lines below.
 */
static const char server_ca_cert[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC9jCCAd6gAwIBAgIUYbMG4Ft3V/YL9vgLaVm7bcin5UIwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMdnVsY2FuLmxvY2FsMB4XDTI2MDMwNjA0NDM0OVoXDTM2\n"
    "MDMwMzA0NDM0OVowFzEVMBMGA1UEAwwMdnVsY2FuLmxvY2FsMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlixQrPFelNlWO488zPvy5yd5bg5EXyNpA2yP\n"
    "9n5AZaKTUxzmEuanWBZzHShGr5SzPKJhI+BCGNoXSHbPVnSQ/nb9pvskAbTm/XMT\n"
    "+17VHMTjImOhka5c39XLXyniyVKMutyRFxsVthBEldT28Hb8kGf+/2qOXIy5jsvC\n"
    "L311fTqD3PVapmEkbHl8DoXpCEg/wZk4hFSIDL7+5U91zPqqPBQHmXt4A6j6ajtm\n"
    "6zB7egxy8d3sxlvJ0AML2rsJfhrQro7iUjjGvEocb15/SZ9tsYveTgAsnKxeK7Ar\n"
    "GEthNJRHT8oodePUCo1my3Q0wB3jgxQHFgp+RI54d7NMgfCrDQIDAQABozowODAX\n"
    "BgNVHREEEDAOggx2dWxjYW4ubG9jYWwwHQYDVR0OBBYEFG8TCpop4U0jqb/eEozk\n"
    "e/i/1C5yMA0GCSqGSIb3DQEBCwUAA4IBAQAOWDQUbgLj5KvvLIuJ00FE4v+39xOo\n"
    "NQO5IYyFDl9Oj13KMVVYIR9lDmphHJvP8m3dAFr4/0Ml3t6dN45wOBKq5CBmlveU\n"
    "bY3X72BsHaDf/SEX/KK7Q3DJvyMlWMSWLlGq+7gdnktBoDmHTmPO8Vh0sqGCZhlO\n"
    "UaR2ZxDPdFoaNdYI6FB+m3xyn9l6pJLzB7XHGYvnHKgEQXwSDQjA9ZhIvaWIIV0F\n"
    "FtUvVBpDyKoMpckt327TfgZqV1hSsgg2oc/mskfUkBPw++nmySV+nNaygZCsDBao\n"
    "bSsZIFi/2MtoJW0IzmOyJgv5b7pk1ZTeCJPXMVVK0IHuTl9JNwa3UBS4\n"
    "-----END CERTIFICATE-----\n";
