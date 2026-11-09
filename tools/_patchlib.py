# -*- coding: utf-8 -*-
"""换行无关的精确替换：文件先统一成 LF 做匹配，写回时用文件原本占多数的换行风格。
（工程里的源码是 CRLF 为主、夹着编辑工具写入的 LF 行，按原样匹配多行锚点必失败。）"""
import io
import re
import sys


class P:
    def __init__(self, path):
        self.path = path
        raw = io.open(path, encoding='utf-8', newline='').read()
        crlf = raw.count('\r\n')
        lf_only = raw.count('\n') - crlf
        self.nl = '\r\n' if crlf >= lf_only else '\n'
        self.s = raw.replace('\r\n', '\n')

    def rep(self, old, new, cnt=1):
        c = self.s.count(old)
        if c != cnt:
            sys.exit('%s: anchor count %d (want %d): %r' % (self.path, c, cnt, old[:80]))
        self.s = self.s.replace(old, new)
        return self

    def rex(self, pat, new, cnt=1):
        # new 是字面量（lambda 替换，不做模板处理）：不能写反向引用 / 转义（如 \1、\n）
        assert '\\1' not in new and '\\2' not in new, 'rex(): new 是字面量，不支持反向引用'
        m = re.findall(pat, self.s, re.S)
        if len(m) != cnt:
            sys.exit('%s: regex count %d (want %d): %r' % (self.path, len(m), cnt, pat[:80]))
        self.s = re.sub(pat, lambda _: new, self.s, flags=re.S)
        return self

    def save(self):
        io.open(self.path, 'w', encoding='utf-8', newline='').write(self.s.replace('\n', self.nl))
        print(self.path, 'ok (%s)' % ('CRLF' if self.nl == '\r\n' else 'LF'))
