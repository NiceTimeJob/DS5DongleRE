/* 网页 js 的静态检查：顶层 const / let / class 声明的名字**不会挂到 window 上**，
 * 所以 `window.GameCfg` 这种写法永远是 undefined。r50~r57 的 page-map.js 就是用
 * `if (window.GameCfg) GameCfg.onDeviceMap(...)` 通知游戏配置卡片的 —— 一次都没通知到，
 * 配置名一次都没写进接收机（2026-10-16 用户实测发现）。要判断"那个模块在不在"用 `typeof X`。
 * 用法：node DongleRE/tools/test_no_window_const.js */
'use strict';
const fs = require('fs');
const path = require('path');

const WEB = path.join(__dirname, '..', 'webgui');
const files = fs.readdirSync(path.join(WEB, 'js')).filter(f => f.endsWith('.js'))
  .map(f => path.join(WEB, 'js', f))
  .concat(fs.readdirSync(WEB).filter(f => f.endsWith('.html')).map(f => path.join(WEB, f)));

/* 收集 js/*.js 里的顶层词法声明（行首、不缩进） */
const names = new Set();
for (const f of files.filter(f => f.endsWith('.js'))) {
  for (const line of fs.readFileSync(f, 'utf8').split('\n')) {
    const m = /^(?:const|let|class)\s+([A-Za-z_$][\w$]*)/.exec(line);
    if (m) names.add(m[1]);
  }
}

let bad = 0;
for (const f of files) {
  const lines = fs.readFileSync(f, 'utf8').split('\n');
  lines.forEach((line, i) => {
    const code = line.replace(/\/\/.*$/, '');
    for (const n of names) {
      /* 注释里提到它（说明这个坑）不算；只查代码 */
      if (new RegExp('(^|[^\\w$.])window\\.' + n.replace('$', '\\$') + '\\b').test(code)
          && !/^\s*(\*|\/\*)/.test(line)) {
        console.log(`FAIL ${path.relative(WEB, f)}:${i + 1}  window.${n} —— ${n} 是顶层 const/let/class，`
                    + `不在 window 上，永远是 undefined（用 typeof ${n} !== 'undefined'）`);
        bad++;
      }
    }
  });
}
console.log(`检查了 ${files.length} 个文件、${names.size} 个顶层名字`);
console.log(bad ? `有 ${bad} 处 window.<顶层名字>` : '全部通过');
process.exit(bad ? 1 : 0);
