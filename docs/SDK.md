# AnnyOS SDK preview

The current SDK preview targets the interpreted `.AP` v1 format. It contains
the public API and format headers, the AP packer, format documentation, and
example application sources. It does not include a native x86 compiler ABI;
that requires a future user-mode process and syscall design.

To package an application on a host with Python 3:

```sh
python3 tools/ap_pack.py apps/HELLO.apasm HELLO.AP
```

AP programs may write text, draw aligned help rows, or call the documented
version 1 system services. The API version in each AP header must be supported
by the target AnnyOS kernel.
