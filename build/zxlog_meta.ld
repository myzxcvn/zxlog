/* GCC 12 ignores section attributes on template-local variables (PR c++/88061).
 * With -fdata-sections it emits our blobs as .rodata.<mangled zxlog_site_>.
 * Collect those before the default .rodata wildcard, alongside the explicitly
 * named sections emitted by compilers that honor ZXLOG_SITE_ATTR. Keep the
 * variable name in logger.h in sync with the mangled-name suffix below.
 */
SECTIONS
{
  zxlog_meta : SUBALIGN(1)
  {
    KEEP(*(zxlog_meta))
    KEEP(*(.rodata.*11zxlog_site_ .rodata.*11zxlog_site_.*))
  }
}
INSERT BEFORE .rodata;
