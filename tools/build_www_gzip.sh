#!/usr/bin/env bash
# ============================================================================
# build_www_gzip.sh —— 对 www 静态资源生成 .gz 孪生文件(drogon gzip_static 前置)
#
# 用法:  bash tools/build_www_gzip.sh [www目录]
#        默认目录: 脚本所在目录的上两级 + www(A:\ZiMo\ZiMoService\www)
#
# 背景(drogon 1.9.13 实测行为):
#   app().setGzipStatic(true) 时,静态文件路由在客户端带 Accept-Encoding: gzip
#   的请求优先发送 <file>.gz,并自动补 Content-Encoding: gzip(浏览器透明解压)。
#   该开关【不在线压缩】,必须预先存在 .gz 孪生文件,否则照发原文件(无副作用)。
#   与 net_dock.cpp 的 opts.gzipStatic=true 配套;服务加载 exe 同级 www,
#   故对同步后的【最终 www 目录】执行(自测: 先同步开发目录 www,后跑本脚本)。
#
# 范围: 仅文本类(压缩率高的 html/css/js/json/svg/...)生成孪生;
#       图片/音频/视频/zip 等已压缩格式跳过(无害无收益,省磁盘)。
# 特性: -9  最高压缩; -n  去文件名/时间戳头(确定性构建,不影响 304 条件请求);
#       -k  保留源文件;增量: 孪生已存在且不早于源文件 → 跳过。
# ============================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WWW_DIR="${1:-$(cd "$SCRIPT_DIR/.." && pwd)/www}"

if [ ! -d "$WWW_DIR" ]; then
    echo "错误: www 目录不存在: $WWW_DIR" >&2
    exit 1
fi

echo "扫描 $WWW_DIR ..."
count=0; saved=0; total=0
while IFS= read -r -d '' f; do
    gz="$f.gz"
    # 增量: 孪生存在且不早于源文件 → 跳过。
    # 用 stat %y 全精度(纳秒)字符串比较;msys2 的 test -nt 按秒截断,
    # 同一秒内生成的 .gz 会被误判为"不新"而重复压缩。
    if [ -f "$gz" ]; then
        gz_m=$(stat -c %y "$gz" 2>/dev/null || true)
        src_m=$(stat -c %y "$f" 2>/dev/null || true)
        if [ -n "$gz_m" ] && [ -n "$src_m" ] && [ "$gz_m" \> "$src_m" ]; then
            continue
        fi
    fi
    src=$(stat -c %s "$f" 2>/dev/null || stat -f %z "$f")
    # -9 -n -k: 最高压缩 / 确定性头 / 保留源文件;重定向写孪生
    gzip -9 -n -k -c "$f" > "$gz"
    dst=$(stat -c %s "$gz" 2>/dev/null || stat -f %z "$gz")
    count=$((count+1))
    saved=$((saved + src - dst))
    total=$((total + src))
done < <(find "$WWW_DIR" -type f \
        \( -iname "*.html" -o -iname "*.htm" -o -iname "*.css" -o \
           -iname "*.js"  -o -iname "*.mjs" -o -iname "*.json" -o \
           -iname "*.xml" -o -iname "*.svg" -o -iname "*.txt" -o \
           -iname "*.md"  -o -iname "*.csv" \) \
        -not -name "*.gz" -print0)

echo "完成: 生成 $count 个 .gz 孪生"
if [ "$total" -gt 0 ]; then
    printf "原文件合计 %d 字节, 压缩后节省 %d 字节 (%.1f%%)\n" \
        "$total" "$saved" "$(awk -v s="$saved" -v t="$total" 'BEGIN{print s*100/t}')"
fi
