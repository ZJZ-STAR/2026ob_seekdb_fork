#include "sql/engine/basic/ob_file_scan_utils.h"
#include <dirent.h>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>

using namespace oceanbase::sql::file_sql;
class FileScanTest : public ::testing::Test {
protected:
  std::string root;
  void SetUp() override {
    char pattern[] = "/tmp/seekdb-file-sql-XXXXXX";
    char *p = mkdtemp(pattern);
    ASSERT_NE(p, nullptr);
    root = p;
  }
  void TearDown() override {
    DIR *d = opendir(root.c_str());
    if (d) {
      while (auto *e = readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..")
          unlink((root + "/" + n).c_str());
      }
      closedir(d);
    }
    rmdir(root.c_str());
  }
  std::string write(const std::string &name, const std::string &body) {
    std::string p = root + "/" + name;
    std::ofstream f(p, std::ios::binary);
    f << body;
    f.close();
    return p;
  }
  size_t count(Reader &r) {
    std::vector<Cell> row;
    bool eof = false;
    size_t n = 0;
    while (!eof) {
      if (!r.next(row, eof)) {
        ADD_FAILURE() << r.input.error;
        break;
      }
      n += !eof;
    }
    return n;
  }
};

TEST_F(FileScanTest, CsvHeadersQuotesNewlinesUtf8AndNull) {
  auto p = write("a.csv", "id,name,id,note,empty\r\n1,中文,2,\"a,\r\nb\",\r\n2,"
                          "B,3,\"say \"\"hi\"\"\",\"\"\r\n");
  Reader r;
  ASSERT_TRUE(r.open(p, root, Options())) << r.input.error;
  ASSERT_EQ(5U, r.columns.size());
  EXPECT_EQ("id__2", r.columns[2].name);
  EXPECT_EQ(INT, r.columns[0].type);
  EXPECT_EQ(TEXT, r.columns[4].type);
  EXPECT_TRUE(r.columns[4].nullable);
  std::vector<Cell> row;
  bool eof;
  ASSERT_TRUE(r.next(row, eof));
  EXPECT_EQ("a,\nb", row[3].text);
  EXPECT_TRUE(row[4].null);
  ASSERT_TRUE(r.next(row, eof));
  EXPECT_EQ("say \"hi\"", row[3].text);
  EXPECT_FALSE(row[4].null);
  EXPECT_EQ("", row[4].text);
  ASSERT_TRUE(r.rescan());
  EXPECT_EQ(2U, count(r));
}
TEST_F(FileScanTest, CsvOptionsAndNoHeader) {
  Options o;
  o.header = false;
  o.delimiter = '|';
  o.quote = '\'';
  o.escape = '\\';
  o.nullstr = "NA";
  o.all_varchar = true;
  auto p = write("o.csv", "1|'a\\'b'|NA\n2|'x|y'|3\n");
  Reader r;
  ASSERT_TRUE(r.open(p, root, o)) << r.input.error;
  EXPECT_EQ("column1", r.columns[0].name);
  EXPECT_EQ(TEXT, r.columns[0].type);
  std::vector<Cell> row;
  bool eof;
  ASSERT_TRUE(r.next(row, eof));
  EXPECT_EQ("a'b", row[1].text);
  EXPECT_TRUE(row[2].null);
  EXPECT_EQ(1U, count(r));
}
TEST_F(FileScanTest, CsvBadRecordAndInvalidUtf8) {
  for (const auto &body :
       std::vector<std::string>{"a,b\n1\n", "a\n\"unterminated", "a\n\"x\"z\n",
                                std::string("a\n") + char(0xff) + "\n"}) {
    Reader r;
    EXPECT_FALSE(r.open(write("bad.csv", body), root, Options()));
    EXPECT_FALSE(r.input.error.empty());
  }
}
TEST_F(FileScanTest, DeterministicTypesAndBoundaries) {
  int64_t n;
  double d;
  EXPECT_TRUE(integer("9223372036854775807", n));
  EXPECT_TRUE(integer("-9223372036854775808", n));
  EXPECT_FALSE(integer("9223372036854775808", n));
  EXPECT_EQ(TEXT, infer(Cell("9223372036854775808")));
  EXPECT_EQ(TEXT, infer(Cell("1e309")));
  EXPECT_FALSE(real("nan", d));
  EXPECT_FALSE(real("0x1p3", d));
  EXPECT_EQ(DATE, infer(Cell("2024-02-29")));
  EXPECT_EQ(TEXT, infer(Cell("2023-02-29")));
  EXPECT_EQ(TEXT, infer(Cell("1900-02-29")));
  EXPECT_EQ(DATE, infer(Cell("2000-02-29")));
  EXPECT_EQ(DATETIME, infer(Cell("2026-10-26T01:02:03.123456")));
  EXPECT_EQ(TEXT, infer(Cell("2026-01-01 24:00:00")));
  EXPECT_EQ(REAL, widen(INT, REAL));
  EXPECT_EQ(DATETIME, widen(DATE, DATETIME));
  EXPECT_EQ(TEXT, widen(BOOL, INT));
  auto p = write("types.csv",
                 "n,d,t,b,z\n1,2024-02-29,2026-01-01 "
                 "00:00:00,true,\n1.5,2026-01-01,2026-01-02T12:34:56,false,\n");
  Reader r;
  ASSERT_TRUE(r.open(p, root, Options()));
  EXPECT_EQ(REAL, r.columns[0].type);
  EXPECT_EQ(DATE, r.columns[1].type);
  EXPECT_EQ(DATETIME, r.columns[2].type);
  EXPECT_EQ(BOOL, r.columns[3].type);
  EXPECT_EQ(NIL, r.columns[4].type);
}
TEST_F(FileScanTest, JsonUnionMissingNullWideningAndUnicode) {
  Options o;
  o.format = "jsonl";
  Reader r;
  auto p =
      write("a.jsonl",
            "{\"id\":1,\"v\":1,\"name\":\"\\u4e2d\\u6587\"}\n{\"id\":2,\"v\":1."
            "5,\"active\":true}\n{\"id\":3,\"name\":null,\"active\":false}\n");
  ASSERT_TRUE(r.open(p, root, o)) << r.input.error;
  ASSERT_EQ(4U, r.columns.size());
  EXPECT_EQ(REAL, r.columns[1].type);
  EXPECT_TRUE(r.columns[2].nullable);
  EXPECT_TRUE(r.columns[3].nullable);
  std::vector<Cell> row;
  bool eof;
  ASSERT_TRUE(r.next(row, eof));
  EXPECT_EQ("中文", row[2].text);
  EXPECT_TRUE(row[3].null);
  ASSERT_TRUE(r.rescan());
  EXPECT_EQ(3U, count(r));
}
TEST_F(FileScanTest, JsonRejectsNestedMalformedAndDuplicateKeys) {
  Options o;
  o.format = "jsonl";
  for (const auto &s : std::vector<std::string>{
           "{\"x\":{}}", "{\"x\":[]}", "[]", "{\"x\":1,\"x\":2}", "{\"x\":01}",
           "{\"x\":1.}", "{\"x\":\"\\uD800\"}", "{\"x\":true} garbage"}) {
    Reader r;
    EXPECT_FALSE(r.open(write("bad.jsonl", s), root, o)) << s;
    EXPECT_FALSE(r.input.error.empty());
  }
}
TEST_F(FileScanTest, FileChangedBeforeOpenDuringScanAndRescan) {
  auto p = write("change.csv", "a\n1\n2\n");
  Reader r;
  ASSERT_TRUE(r.open(p, root, Options()));
  std::string plan = descriptor(r);
  write("change.csv", "a\n3\n4\n");
  Reader cached;
  EXPECT_FALSE(restore(cached, plan, root));
  std::vector<Cell> row;
  bool eof;
  EXPECT_FALSE(r.next(row, eof));
  EXPECT_FALSE(r.rescan());
}
TEST_F(FileScanTest, PathTraversalSymlinksAndSpecialFiles) {
  auto p = write("a.csv", "a\n1\n");
  Reader a;
  EXPECT_FALSE(a.open(p, "", Options()));
  Reader b;
  EXPECT_FALSE(b.open("/etc/passwd", root, Options()));
  ASSERT_EQ(0, symlink("/etc/passwd", (root + "/out.csv").c_str()));
  Reader c;
  EXPECT_FALSE(c.open(root + "/out.csv", root, Options()));
  ASSERT_EQ(0, mkfifo((root + "/pipe.csv").c_str(), 0600));
  Reader d;
  EXPECT_FALSE(d.open(root + "/pipe.csv", root, Options()));
  Reader e;
  EXPECT_FALSE(e.open(root + "/../outside.csv", root, Options()));
  Reader f;
  EXPECT_FALSE(f.open(p + std::string("\0junk", 5), root, Options()));
}
TEST_F(FileScanTest, GlobPatternRescanAndDirectoryChange) {
  write("a.csv", "a\n1\n");
  write("b.txt", "b");
  Options o;
  o.format = "glob";
  Reader r;
  ASSERT_TRUE(r.open(root + "/*.csv", root, o)) << r.input.error;
  EXPECT_EQ(1U, count(r));
  ASSERT_TRUE(r.rescan());
  EXPECT_EQ(1U, count(r));
  Reader restored;
  ASSERT_TRUE(restore(restored, descriptor(r), root)) << restored.input.error;
  EXPECT_EQ(1U, count(restored));
  write("new.csv", "a\n2\n");
  EXPECT_FALSE(r.rescan());
  Reader outside;
  EXPECT_FALSE(outside.open("/etc/*", root, o));
}
TEST_F(FileScanTest, StreamingLargeCsv) {
  auto p = write("large.csv", "id,name\n");
  {
    std::ofstream f(p, std::ios::app);
    for (int i = 0; i < 100000; ++i)
      f << i << ",same\n";
  }
  Reader r;
  ASSERT_TRUE(r.open(p, root, Options())) << r.input.error;
  EXPECT_EQ(100000U, count(r));
  ASSERT_TRUE(r.rescan());
  EXPECT_EQ(100000U, count(r));
}
TEST_F(FileScanTest, BoundedBatchReadsAndRescan) {
  auto p = write("batch.csv", "id\n1\n2\n3\n4\n5\n");
  Reader r;
  ASSERT_TRUE(r.open(p, root, Options()));
  std::vector<std::vector<Cell>> batch;
  bool eof;
  ASSERT_TRUE(r.next_batch(3, batch, eof));
  EXPECT_EQ(3U, batch.size());
  EXPECT_FALSE(eof);
  ASSERT_TRUE(r.next_batch(3, batch, eof));
  EXPECT_EQ(2U, batch.size());
  EXPECT_TRUE(eof);
  ASSERT_TRUE(r.rescan());
  ASSERT_TRUE(r.next_batch(5, batch, eof));
  EXPECT_EQ("1", batch[0][0].text);
  EXPECT_EQ("5", batch[4][0].text);
  EXPECT_FALSE(r.next_batch(0, batch, eof));
}
TEST_F(FileScanTest, ParquetAllFormatsMultiGroupAndRescan) {
  const char *dir = getenv("FILE_SQL_FIXTURES");
  ASSERT_NE(dir, nullptr);
  Options o;
  o.format = "parquet";
  for (const char *f : {"flat_v1_none.parquet", "flat_v1_snappy.parquet",
                        "flat_v2_none.parquet", "flat_v2_snappy.parquet",
                        "flat_plain.parquet", "flat_int96.parquet"}) {
    Reader r;
    std::string path = std::string(dir) + "/" + f;
    ASSERT_TRUE(r.open(path, dir, o)) << f << ": " << r.input.error;
    ASSERT_EQ(6U, r.columns.size());
    EXPECT_EQ(3U, r.parquet.groups.size());
    std::vector<Cell> row;
    bool eof;
    ASSERT_TRUE(r.next(row, eof)) << r.input.error;
    EXPECT_EQ("1", row[0].text);
    EXPECT_EQ("1.5", row[1].text);
    EXPECT_EQ("true", row[2].text);
    EXPECT_EQ("alpha", row[3].text);
    EXPECT_EQ(6U, count(r)) << f;
    ASSERT_TRUE(r.rescan());
    r.projected = {true, false, false, false, false, false};
    EXPECT_EQ(7U, count(r)) << f;
  }
}
TEST_F(FileScanTest, ParquetMultiplePagesCorruptionAndNested) {
  const char *dir = getenv("FILE_SQL_FIXTURES");
  ASSERT_NE(dir, nullptr);
  Options o;
  o.format = "parquet";
  Reader pages;
  ASSERT_TRUE(pages.open(std::string(dir) + "/many_pages.parquet", dir, o))
      << pages.input.error;
  EXPECT_EQ(256U, count(pages));
  Reader nested;
  EXPECT_FALSE(nested.open(std::string(dir) + "/nested.parquet", dir, o));
  for (const auto &s : std::vector<std::string>{"", "PAR1", "PAR1garbagePAR1",
                                                "not a parquet"}) {
    Reader bad;
    EXPECT_FALSE(bad.open(write("bad.parquet", s), root, o));
  }
}
TEST_F(FileScanTest, CopyRoundTripNoOverwriteAndFailureCleanup) {
  std::string config = "FSQLCOPY1";
  config += ',';
  config += '"';
  config += '"';
  config += char(1);
  put(config, "NULL");
  auto p = root + "/export.csv";
  CsvExport writer;
  ASSERT_TRUE(writer.open(p, root, config)) << writer.error;
  ASSERT_TRUE(writer.row({Cell("id"), Cell("name"), Cell("v")}));
  ASSERT_TRUE(writer.row({Cell("1"), Cell("say \"hi\",\n中文"), Cell()}));
  ASSERT_TRUE(writer.row({Cell("2"), Cell("NULL"), Cell("")}));
  ASSERT_TRUE(writer.commit()) << writer.error;
  CsvExport second;
  EXPECT_FALSE(second.open(p, root, config));
  Options o;
  o.nullstr = "NULL";
  Reader r;
  ASSERT_TRUE(r.open(p, root, o)) << r.input.error;
  std::vector<Cell> row;
  bool eof;
  ASSERT_TRUE(r.next(row, eof));
  EXPECT_EQ("say \"hi\",\n中文", row[1].text);
  EXPECT_TRUE(row[2].null);
  ASSERT_TRUE(r.next(row, eof));
  EXPECT_EQ("NULL", row[1].text);
  EXPECT_FALSE(row[1].null);
  EXPECT_FALSE(row[2].null);
  {
    CsvExport failed;
    ASSERT_TRUE(failed.open(root + "/failed.csv", root, config));
    ASSERT_TRUE(failed.row({Cell("partial")}));
  }
  EXPECT_NE(0, access((root + "/failed.csv").c_str(), F_OK));
  DIR *d = opendir(root.c_str());
  ASSERT_NE(d, nullptr);
  while (auto *e = readdir(d))
    EXPECT_NE(0, std::string(e->d_name).find(".seekdb-copy-"));
  closedir(d);
}
TEST_F(FileScanTest, BomJsonStringConflictsAndRecordLocations) {
  Reader csv;
  ASSERT_TRUE(csv.open(write("bom.csv", "\xef\xbb\xbfid,name\n1,中文\n"), root,
                       Options()));
  EXPECT_EQ("id", csv.columns[0].name);
  Options o;
  o.format = "jsonl";
  Reader json;
  ASSERT_TRUE(
      json.open(write("strings.jsonl", "{\"v\":1}\n{\"v\":\"2\"}\n"), root, o));
  EXPECT_EQ(TEXT, json.columns[0].type);
  Reader bad;
  EXPECT_FALSE(bad.open(write("bad.csv", "a,b\n1\n"), root, Options()));
  EXPECT_NE(std::string::npos, bad.input.error.find("line 2:"));
}
TEST_F(FileScanTest, ParquetProjectionSkipsCorruptedUnreferencedColumn) {
  const char *dir = getenv("FILE_SQL_FIXTURES");
  ASSERT_NE(dir, nullptr);
  std::ifstream source(std::string(dir) + "/flat_plain.parquet",
                       std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(source)),
                    std::istreambuf_iterator<char>());
  Options o;
  o.format = "parquet";
  Reader schema;
  auto path = write("projection.parquet", bytes);
  ASSERT_TRUE(schema.open(path, root, o));
  ASSERT_GT(schema.parquet.groups[0].chunks.size(), 1U);
  bytes[schema.parquet.groups[0].chunks[1].offset] = char(0xff);
  write("projection.parquet", bytes);
  Reader projected;
  ASSERT_TRUE(projected.open(path, root, o));
  projected.projected = {true, false, false, false, false, false};
  EXPECT_EQ(7U, count(projected));
  Reader all;
  ASSERT_TRUE(all.open(path, root, o));
  std::vector<Cell> row;
  bool eof;
  EXPECT_FALSE(all.next(row, eof));
}
TEST_F(FileScanTest, MutatedParquetDoesNotCrash) {
  const char *dir = getenv("FILE_SQL_FIXTURES");
  ASSERT_NE(dir, nullptr);
  std::ifstream source(std::string(dir) + "/flat_v2_snappy.parquet",
                       std::ios::binary);
  std::string original((std::istreambuf_iterator<char>(source)),
                       std::istreambuf_iterator<char>());
  ASSERT_GT(original.size(), 100U);
  Options o;
  o.format = "parquet";
  uint32_t random = 0x13572468;
  for (int i = 0; i < 200; ++i) {
    std::string bytes = original;
    random = random * 1664525U + 1013904223U;
    size_t p = random % bytes.size();
    bytes[p] ^= char((random >> 24) | 1);
    Reader r;
    if (r.open(write("mutated.parquet", bytes), root, o)) {
      std::vector<Cell> row;
      bool eof = false;
      for (int n = 0; n < 512 && !eof; ++n)
        if (!r.next(row, eof))
          break;
    }
  }
}
