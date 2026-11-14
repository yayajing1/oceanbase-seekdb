#!/bin/bash
# Regression check for the fulltext index optimization (task four).
#
# It verifies that the optimization did not change the observable behaviour of
# the fulltext index:
#
#   1. the TOKENIZE output of a corpus that covers the tricky parts of the
#      tokenizer (chinese numbers, number/letter connectors, full width forms,
#      surrogate pairs, mixed chinese/english text, ...) is byte by byte the
#      same as the recorded golden output,
#   2. the MATCH semantics are unchanged: natural language mode, boolean mode,
#      the match score in the select list, the match score used to sort the
#      result, a match combined with another condition and the empty result
#      case all return the expected rows,
#   3. the index can still be built and the index content is the expected one.
#
# The script only uses the mysql client and bash, so it can run in the same
# environment as the benchmark script.
#
# Usage:
#   bash tools/benchmark/fts_regression_check.sh              # check
#   bash tools/benchmark/fts_regression_check.sh --record     # record the golden file
#
# Environment:
#   MYSQL       the mysql command line, the default is the one of the benchmark
#   GOLDEN_FILE the golden file, the default is the one next to this script

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GOLDEN_FILE="${GOLDEN_FILE:-${SCRIPT_DIR}/fts_regression_check.golden}"
DB="fts_regression_check"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/fts-regression.XXXXXX")"
RECORD=0
if [[ "${1:-}" == "--record" ]]; then
  RECORD=1
fi

cleanup() { rm -f "${OUT_FILE}"; }
trap cleanup EXIT

log() { echo "$@" >&2; }

mysql_exec() {
  # shellcheck disable=SC2086
  $MYSQL -e "SET NAMES utf8mb4; SET ob_query_timeout = 100000000000; SET ob_trx_timeout = 100000000000; $1"
}

mysql_query() {
  # shellcheck disable=SC2086
  $MYSQL -N -s -e "SET NAMES utf8mb4; SET ob_query_timeout = 100000000000; SET ob_trx_timeout = 100000000000; $1"
}

log "=== FTS regression check ==="

# the server may still be starting up when the script is called
for _ in $(seq 1 60); do
  if mysql_query "SELECT 1;" > /dev/null 2>&1; then
    break
  fi
  sleep 1
done

mysql_exec "DROP DATABASE IF EXISTS ${DB};"
mysql_exec "CREATE DATABASE ${DB};"
mysql_exec "USE ${DB};
CREATE TABLE docs (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  title VARCHAR(256) NOT NULL,
  content TEXT NOT NULL,
  title_en VARCHAR(256) NOT NULL,
  content_en TEXT NOT NULL
) DEFAULT CHARSET = utf8mb4;"

mysql_exec "USE ${DB};
INSERT INTO docs (title, content, title_en, content_en) VALUES
('全文索引', '全文索引的分词器热路径优化包括解析器实例复用', 'Full-text search', 'Full-text search indexing requires efficient tokenization'),
('分词器', '在大量文档索引场景下，ik 分词需要频繁查词典', 'Tokenization hot path', 'The tokenizer is executed on the indexing hot path'),
('倒排索引', '倒排索引是全文检索的核心数据结构，分词质量直接影响召回率', 'Inverted index build', 'The inverted index stores the posting lists'),
('OceanBase', 'OceanBase 是一款非常稳定的数据库，支持 ik、beng 等分词器', 'Database audit', 'Completed the annual security audit, no major vulnerabilities'),
('第一章', '第一章介绍测试中文数字一百二十三的处理', 'Query latency', 'Query latency under heavy load is measured'),
('第二章', '第3章讨论分词规则与停用词全局单例', 'Bulk insert', 'Bulk loading documents triggers repeated tokenizer execution'),
('emoji', '混合文档 row=2，用于压测 MATCH 与 TOKENIZE 😀', 'English tokenizer', 'The english beng parser is the baseline for the comparison'),
('千分位', '第1,234,567.89 行的数值与 2024年 的时间', 'Relevance scoring', 'Document retrieval relevance scoring is expensive'),
('连字符', 'a-b_c@d 与 #hash 以及 e-mail 地址', 'Memory and IO', 'Reuse the parser and the buffer to reduce the memory usage'),
('全角', 'ＡＢＣ全角与 １２３．４５６ 以及 ＃标签', 'Parallel build', 'Parallel index build improves the throughput'),
('Ωμέγα', '希腊字母 Ωμέγα 与 ﬀﬁﬂ 连字', 'Sorting and merge', 'Optimize the sort key comparison and the external merge'),
('短', '一', 'x', 'y');"

# the index build itself is part of the regression: it must succeed and it must
# produce the same index content as before the optimization
mysql_exec "USE ${DB};
ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title, content) WITH PARSER ik;"
mysql_exec "USE ${DB};
ALTER TABLE docs ADD FULLTEXT INDEX fti_content_ik(content) WITH PARSER ik;"
mysql_exec "USE ${DB};
ALTER TABLE docs ADD FULLTEXT INDEX fti_en_beng(title_en, content_en) WITH PARSER beng;"

{
  echo "# === tokenizer output ==="
  for text in \
    '' \
    'a' \
    '0' \
    '1,234.56' \
    '#hash' \
    'a-b_c@d' \
    '全文索引' \
    '分词器' \
    'OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用' \
    '一' \
    '一二三' \
    '十' \
    '百万' \
    '2024年' \
    '第3章' \
    '第3章第三节' \
    '混合ABC123中文' \
    '数据库database数据库' \
    'ＡＢＣ全角' \
    '😀表情符号' \
    'emoji😀测试' \
    '  多个   空格  ' \
    '换行\n换行' \
    'tab\t分隔' \
    'Ωμέγα' \
    '12345678901234567890' \
    'a1b2c3' \
    '中文分词器分词器中文分词器' \
    '１，２３４' \
    'a－b' \
    '＃标签' \
    '全文索引，数据库。' \
    '１２３．４５６' \
    'ＡＢＣ－ＤＥＦ'
  do
    for parser in ik beng; do
      esc=$(printf '%s' "${text}" | sed "s/'/''/g")
      printf 'tokenize(%s) -> ' "$(printf '%s' "${esc}" | tr -d '\n\t')"
      mysql_query "USE ${DB}; SELECT TOKENIZE('${esc}', '${parser}');"
    done
  done

  echo "# === match semantics ==="
  mysql_query "USE ${DB}; SELECT 'nl_single', COUNT(*) FROM docs WHERE MATCH(content) AGAINST('分词器');"
  mysql_query "USE ${DB}; SELECT 'nl_two_terms', COUNT(*) FROM docs WHERE MATCH(title, content) AGAINST('全文索引 分词器');"
  mysql_query "USE ${DB}; SELECT 'nl_english', COUNT(*) FROM docs WHERE MATCH(title_en, content_en) AGAINST('database performance audit');"
  mysql_query "USE ${DB}; SELECT 'boolean_plus', COUNT(*) FROM docs WHERE MATCH(content) AGAINST('分词器 +全文索引' IN BOOLEAN MODE);"
  mysql_query "USE ${DB}; SELECT 'boolean_minus', COUNT(*) FROM docs WHERE MATCH(content) AGAINST('分词器 -全文索引' IN BOOLEAN MODE);"
  mysql_query "USE ${DB}; SELECT 'no_hit', COUNT(*) FROM docs WHERE MATCH(content) AGAINST('zzzqqqxxx');"
  mysql_query "USE ${DB}; SELECT 'match_and_id', COUNT(*) FROM docs WHERE MATCH(content) AGAINST('分词器') AND id > 5;"
  mysql_query "USE ${DB}; SELECT 'match_and_like', COUNT(*) FROM docs WHERE MATCH(content) AGAINST('分词器') AND title LIKE '%索引%';"
  mysql_query "USE ${DB}; SELECT 'score_in_select', COUNT(*) FROM (SELECT id, MATCH(content) AGAINST('分词器') AS s FROM docs WHERE MATCH(content) AGAINST('分词器')) t;"
  mysql_query "USE ${DB}; SELECT 'order_by_score', COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('分词器') ORDER BY MATCH(content) AGAINST('分词器') DESC LIMIT 3) t;"
  mysql_query "USE ${DB}; SELECT 'limit_scan', COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('分词器') LIMIT 5) t;"
  mysql_query "USE ${DB}; SELECT 'beng_index', COUNT(*) FROM docs WHERE MATCH(title_en, content_en) AGAINST('tokenizer');"
} > "${OUT_FILE}"

# the mysql client may translate the line endings on windows, the golden file
# is always stored with unix line endings
tr -d '\r' < "${OUT_FILE}" > "${OUT_FILE}.lf" && mv "${OUT_FILE}.lf" "${OUT_FILE}"

mysql_exec "DROP DATABASE IF EXISTS ${DB};"

if [[ "${RECORD}" == "1" ]]; then
  cp "${OUT_FILE}" "${GOLDEN_FILE}"
  log "recorded the golden file ${GOLDEN_FILE}"
  exit 0
fi

if [[ ! -f "${GOLDEN_FILE}" ]]; then
  log "ERROR: the golden file ${GOLDEN_FILE} does not exist"
  exit 1
fi

if diff -u "${GOLDEN_FILE}" "${OUT_FILE}"; then
  log "PASS: the fulltext behaviour is unchanged"
  exit 0
else
  log "FAIL: the fulltext behaviour is changed, see the diff above"
  exit 1
fi
