/* Host tests for the LDAP Plug-In's helpers (plugins/ldap/ldaputil.c): BER lengths and
   integers (X.690 8.1.3, 8.3), a bind, search, and unbind byte for byte (RFC 1777 4.1, 4.3,
   4.4, 4.5.1), string filters (RFC 1960 2, RFC 2254's escapes), reading a result and an
   entry a few bytes at a time, ldap: addresses (RFC 1959, RFC 2255's extensions), and the
   Name field's filter. */
#include <stdio.h>
#include <string.h>

#include "../plugins/ldap/ldaputil.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int hex_is(const unsigned char *b, long n, const char *hex)
{
    char s[2000];
    long i;
    for (i = 0; i < n && i < 990; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    s[2 * i] = 0;
    if (strcmp(s, hex)) { printf("  got %s\n  not %s\n", s, hex); return 0; }
    return 1;
}

static unsigned char buf[2048];
static LdapOut o;

static void test_basics(void)
{
    long at;
    ldap_out(&o, buf, sizeof buf);
    ldap_put_int(&o, 0x02, 0); ldap_put_int(&o, 0x02, 127); ldap_put_int(&o, 0x02, 128);
    ldap_put_int(&o, 0x02, 256); ldap_put_int(&o, 0x02, -1); ldap_put_int(&o, 0x02, -129); ldap_put_int(&o, 0x0a, 100000);
    CHECK(hex_is(buf, o.n, "020100" "02017f" "02020080" "02020100" "0201ff" "0202ff7f" "0a030186a0"));
    /* a constructed element's length, short and long */
    ldap_out(&o, buf, sizeof buf);
    at = ldap_begin(&o, 0x30);
    ldap_put_str(&o, 0x04, "ab", 2);
    ldap_end(&o, at);
    CHECK(hex_is(buf, o.n, "3004" "04026162"));
    ldap_out(&o, buf, sizeof buf);
    at = ldap_begin(&o, 0x30);
    {
        char v[200];
        memset(v, 'x', sizeof v);
        ldap_put_str(&o, 0x04, v, 200);
    }
    ldap_end(&o, at);
    CHECK(o.n == 3 + 3 + 200 && buf[0] == 0x30 && buf[1] == 0x81 && buf[2] == 203 && buf[3] == 0x04 && buf[4] == 0x81 && buf[5] == 200);
    /* too small: full, not past the end */
    ldap_out(&o, buf, 3);
    ldap_put_str(&o, 0x04, "abcd", 4);
    CHECK(o.full && o.n == 3);
}

static void test_requests(void)
{
    ldap_out(&o, buf, sizeof buf);
    ldap_bind(&o, 1, 2, "cn=x", "pw");
    CHECK(hex_is(buf, o.n, "3012" "020101" "600d" "020102" "0404636e3d78" "80027077"));
    ldap_out(&o, buf, sizeof buf);
    ldap_bind(&o, 2, 3, "", "");
    CHECK(hex_is(buf, o.n, "300c" "020102" "6007" "020103" "0400" "8000"));
    ldap_out(&o, buf, sizeof buf);
    ldap_unbind(&o, 3);
    CHECK(hex_is(buf, o.n, "3005" "020103" "4200"));
    /* base "o=a", one level, 50 at most, (cn=*), cn and mail */
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_search(&o, 4, "o=a", 1, 50, 0, "(cn=*)", " cn , mail,*"));
    CHECK(hex_is(buf, o.n, "3029" "020104" "6324" "04036f3d61" "0a0101" "0a0100" "020132" "020100" "010100"
                           "8702636e" "300a" "0402636e" "04046d61696c"));
    /* types only, every attribute, the default filter */
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_search(&o, 5, "", 2, 1, 1, "", ""));
    CHECK(hex_is(buf, o.n, "3025" "020105" "6320" "0400" "0a0102" "0a0100" "020101" "020100" "0101ff"
                           "870b6f626a656374436c617373" "3000"));
}

static void test_filters(void)
{
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_filter(&o, "(&(objectClass=person)(cn=Sm*th))"));
    CHECK(hex_is(buf, o.n, "a027" "a315" "040b6f626a656374436c617373" "0406706572736f6e"
                           "a40e" "0402636e" "3008" "8002536d" "82027468"));
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_filter(&o, "(|(cn=*a*b*)(!(sn~=x))(age>=3)(age<=9))"));
    {
        /* any [1] twice, no initial or final */
        static const unsigned char want[] = { 0xa1, 0x2d, 0xa4, 0x0c, 0x04, 0x02, 'c', 'n', 0x30, 0x06, 0x81, 0x01, 'a', 0x81, 0x01, 'b',
                                              0xa2, 0x09, 0xa8, 0x07, 0x04, 0x02, 's', 'n', 0x04, 0x01, 'x',
                                              0xa5, 0x08, 0x04, 0x03, 'a', 'g', 'e', 0x04, 0x01, '3',
                                              0xa6, 0x08, 0x04, 0x03, 'a', 'g', 'e', 0x04, 0x01, '9' };
        CHECK(o.n == (long)sizeof want && !memcmp(buf, want, sizeof want));
        if (o.n != (long)sizeof want) printf("  n %ld\n", o.n);
    }
    /* escapes: RFC 2254's \2a, and RFC 1960's \* and \) */
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_filter(&o, "(cn=a\\2ab\\*c\\)d)"));
    CHECK(hex_is(buf, o.n, "a30d" "0402636e" "0407" "612a622a632964"));
    /* without its parentheses */
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_filter(&o, "cn=Smith*"));
    CHECK(hex_is(buf, o.n, "a40d" "0402636e" "3007" "8005536d697468"));
    /* not filters */
    ldap_out(&o, buf, sizeof buf);
    CHECK(!ldap_filter(&o, "(cn=x"));
    CHECK(!ldap_filter(&o, "(&)"));
    CHECK(!ldap_filter(&o, "(=x)"));
    CHECK(!ldap_filter(&o, "(cn=x))"));
    CHECK(!ldap_filter(&o, "(cn<x)"));
}

/* reading: a few bytes at a time */
static const unsigned char *src;
static long src_n, src_at;
static long fill3(LdapIn *in, unsigned char *b, long size)
{
    long k = src_n - src_at;
    (void)in;
    if (k > 3) k = 3;
    if (k > size) k = size;
    memcpy(b, src + src_at, k);
    src_at += k;
    return k;
}

static void reading(const unsigned char *s, long n, LdapIn *in)
{
    src = s;
    src_n = n;
    src_at = 0;
    memset(in, 0, sizeof *in);
    in->fill = fill3;
}

static void test_reading(void)
{
    /* BindResponse: success, matchedDN "", errorMessage "ok", and a v3 referral to skip */
    static const unsigned char bind[] = { 0x30, 0x13, 0x02, 0x01, 0x01, 0x61, 0x0e, 0x0a, 0x01, 0x31, 0x04, 0x00, 0x04, 0x02, 'n', 'o',
                                          0xa3, 0x03, 0x04, 0x01, 'r', 0x30 };
    static const unsigned char entry[] = { 0x30, 0x82, 0x00, 0x1d, 0x02, 0x01, 0x02, 0x64, 0x18, 0x04, 0x03, 'o', '=', 'a',
                                           0x30, 0x11, 0x30, 0x0f, 0x04, 0x02, 'c', 'n', 0x31, 0x09, 0x04, 0x01, 'A', 0x04, 0x04, 0, 1, 2, 3 };
    LdapIn in;
    long len, k;
    char msg[8], v[8];
    reading(bind, sizeof bind, &in);
    CHECK(ldap_tag(&in, &len) == 0x30 && len == 19);
    CHECK(ldap_tag(&in, &len) == 0x02 && ldap_int(&in, len) == 1);
    CHECK(ldap_tag(&in, &len) == 0x61 && len == 14);
    CHECK(ldap_result(&in, len, msg, sizeof msg) == 49 && !strcmp(msg, "no"));
    CHECK(ldap_byte(&in) == 0x30 && !in.bad);      /* the referral was skipped, and no more */
    CHECK(ldap_byte(&in) == 0 && in.bad);
    reading(entry, sizeof entry, &in);
    CHECK(ldap_tag(&in, &len) == 0x30 && len == 29);
    CHECK(ldap_tag(&in, &len) == 0x02 && ldap_int(&in, len) == 2);
    CHECK(ldap_tag(&in, &len) == 0x64 && len == 24);
    CHECK(ldap_tag(&in, &len) == 0x04 && ldap_bytes(&in, len, v, sizeof v) == 3 && !strcmp(v, "o=a"));
    CHECK(ldap_tag(&in, &len) == 0x30 && len == 17);
    CHECK(ldap_tag(&in, &len) == 0x30 && ldap_tag(&in, &len) == 0x04 && ldap_bytes(&in, len, v, 2) == 1 && !strcmp(v, "c"));
    CHECK(ldap_tag(&in, &len) == 0x31 && len == 9);
    CHECK(ldap_tag(&in, &len) == 0x04 && ldap_bytes(&in, len, v, sizeof v) == 1);
    CHECK(ldap_tag(&in, &len) == 0x04 && len == 4);
    k = ldap_bytes(&in, len, v, sizeof v);
    CHECK(k == 4 && !ldap_text((unsigned char *)v, k) && ldap_text((const unsigned char *)"a\tb\r\n", 5));
    CHECK(in.pos == (long)sizeof entry && !in.bad);
    /* a length too long to be one */
    {
        static const unsigned char bad[] = { 0x30, 0x85, 1, 2, 3, 4, 5 };
        reading(bad, sizeof bad, &in);
        CHECK(ldap_tag(&in, &len) == -1 && in.bad);
    }
}

static void test_urls(void)
{
    LdapUrl u;
    char e[100];
    CHECK(ldap_url("/o=University%20of%20Michigan,c=US?postalAddress?base?(objectClass=*)", &u));
    CHECK(!strcmp(u.dn, "o=University of Michigan,c=US") && !strcmp(u.attrs, "postalAddress") && u.scope == kLdapBase &&
          !strcmp(u.filter, "(objectClass=*)") && u.table == -1 && !u.bindname[0]);
    CHECK(ldap_url("/o=a??sub?(cn=Babs%20Jensen)?!bindname=cn=Manager%2co=a,x-layout=table", &u));
    CHECK(!strcmp(u.filter, "(cn=Babs Jensen)") && u.scope == kLdapSub && !strcmp(u.bindname, "cn=Manager,o=a") && u.table == 1);
    CHECK(ldap_url("/", &u) && !u.dn[0] && u.scope == kLdapBase);
    CHECK(ldap_url("/o=a??ONE??x-layout=records", &u) && u.scope == kLdapOne && u.table == 0);
    CHECK(!ldap_url("/o=a??deep", &u));
    CHECK(ldap_escape("o=Caf\xe9 & co?#", e, sizeof e) > 0 && !strcmp(e, "o=Caf%E9%20&%20co%3F%23"));
    ldap_name_filter("Sm(i)th*", e, sizeof e);
    CHECK(!strcmp(e, "(|(cn=Sm\\28i\\29th\\2a*)(sn=Sm\\28i\\29th\\2a*))"));
    ldap_out(&o, buf, sizeof buf);
    CHECK(ldap_filter(&o, e));
}

int main(void)
{
    test_basics();
    test_requests();
    test_filters();
    test_reading();
    test_urls();
    if (failures) { printf("%d ldap check(s) failed\n", failures); return 1; }
    printf("ldap: all passed\n");
    return 0;
}
