/*
   Unit tests for heap_scan() internal continuation record skipping.

   Verifies that heap_scan() skips blob continuation records internally
   (via goto retry) rather than returning HA_ERR_RECORD_DELETED to the
   caller.  Uses real HEAP tables with blob columns.
*/

#include "hp_test_helpers.h"


/*
  Test 1: scan with continuation records never returns HA_ERR_RECORD_DELETED.

  Inserts rows with blobs large enough to create continuation chains
  (recbuffer=16, so >5 bytes needs continuations), then scans and
  verifies that heap_scan returns only 0 or HA_ERR_END_OF_FILE.
*/
static void test_scan_skips_continuations(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar scan_buf[REC_LENGTH];
  int error;
  uint row_count= 0;
  my_bool got_record_deleted= FALSE;

  uchar blob1[50], blob2[80];
  memset(blob1, 'A', sizeof(blob1));
  memset(blob2, 'B', sizeof(blob2));
  blob1[0]= '1'; blob1[49]= 'Z';
  blob2[0]= '2'; blob2[79]= 'Z';

  if (create_and_open("test_scan_cont", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  build_record(rec, 1, blob1, sizeof(blob1));
  ok(heap_write(info, rec) == 0, "insert row 1 (50-byte blob)");

  build_record(rec, 2, blob2, sizeof(blob2));
  ok(heap_write(info, rec) == 0, "insert row 2 (80-byte blob)");

  ok(share->records == 2,
     "records == 2 (got %lu)", (ulong) share->records);
  ok(share->total_records > share->records,
     "total_records (%lu) > records (%lu)",
     (ulong) share->total_records, (ulong) share->records);

  heap_scan_init(info);
  while ((error= heap_scan(info, scan_buf)) != HA_ERR_END_OF_FILE)
  {
    if (error == HA_ERR_RECORD_DELETED)
      got_record_deleted= TRUE;
    else if (error == 0)
      row_count++;
    else
      break;
  }

  ok(!got_record_deleted,
     "heap_scan never returned HA_ERR_RECORD_DELETED for continuations");
  ok(row_count == 2,
     "scan returned 2 rows (got %u)", row_count);

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 2: scan with deleted rows AND continuation records.

  Inserts 3 rows with blobs, deletes the middle one, then scans.
  Deleted rows should still return HA_ERR_RECORD_DELETED (existing
  behavior), but continuation records must be skipped internally.
*/
static void test_scan_deleted_plus_continuations(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar scan_buf[REC_LENGTH];
  int error;
  uint row_count= 0;
  uint deleted_count= 0;

  uchar blob1[40], blob2[60], blob3[45];
  memset(blob1, 'X', sizeof(blob1));
  memset(blob2, 'Y', sizeof(blob2));
  memset(blob3, 'Z', sizeof(blob3));

  if (create_and_open("test_scan_del", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  build_record(rec, 10, blob1, sizeof(blob1));
  ok(heap_write(info, rec) == 0, "insert row 10");

  build_record(rec, 20, blob2, sizeof(blob2));
  ok(heap_write(info, rec) == 0, "insert row 20");

  build_record(rec, 30, blob3, sizeof(blob3));
  ok(heap_write(info, rec) == 0, "insert row 30");

  /* Delete row 20 via key lookup */
  {
    uchar key[4];
    int4store(key, 20);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 20 for deletion");
    ok(heap_delete(info, rec) == 0, "deleted row 20");
  }

  ok(share->records == 2,
     "records == 2 after delete (got %lu)", (ulong) share->records);

  heap_scan_init(info);
  while ((error= heap_scan(info, scan_buf)) != HA_ERR_END_OF_FILE)
  {
    if (error == HA_ERR_RECORD_DELETED)
      deleted_count++;
    else if (error == 0)
      row_count++;
    else
      break;
  }

  ok(row_count == 2, "scan returned 2 live rows (got %u)", row_count);
  ok(deleted_count > 0,
     "scan returned HA_ERR_RECORD_DELETED for deleted slots (%u times)",
     deleted_count);

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 3: scan a non-blob table is unaffected.

  Inserts rows without blobs, scans, verifies existing behavior is
  unchanged (deleted records still return HA_ERR_RECORD_DELETED).
*/
static void test_scan_no_blobs(void)
{
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  HP_CREATE_INFO ci;
  HP_SHARE *share;
  HP_INFO *info;
  my_bool unused;
  uchar rec[REC_LENGTH];
  uchar scan_buf[REC_LENGTH];
  int error;
  uint row_count= 0;
  uint deleted_count= 0;

  memset(&keyseg, 0, sizeof(keyseg));
  keyseg.type=    HA_KEYTYPE_BINARY;
  keyseg.start=   INT_OFFSET;
  keyseg.length=  4;
  keyseg.charset= &my_charset_bin;

  memset(&keydef, 0, sizeof(keydef));
  keydef.keysegs=   1;
  keydef.seg=       &keyseg;
  keydef.algorithm= HA_KEY_ALG_HASH;
  keydef.flag=      HA_NOSAME;
  keydef.length=    4;

  memset(&ci, 0, sizeof(ci));
  ci.keys=           1;
  ci.keydef=         &keydef;
  ci.reclength=      REC_LENGTH;
  ci.max_records=    1000;
  ci.min_records=    10;
  ci.max_table_size= 1024 * 1024;

  if (heap_create("test_scan_noblob", &ci, &share, &unused))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(4, "setup failed");
    return;
  }
  info= heap_open("test_scan_noblob", 2);
  if (!info)
  {
    ok(0, "open failed: %d", my_errno);
    skip(4, "open failed");
    return;
  }

  /* Insert 3 rows (no blob data, just int) */
  memset(rec, 0, REC_LENGTH);
  int4store(rec + INT_OFFSET, 100);
  ok(heap_write(info, rec) == 0, "insert row 100");

  int4store(rec + INT_OFFSET, 200);
  ok(heap_write(info, rec) == 0, "insert row 200");

  int4store(rec + INT_OFFSET, 300);
  ok(heap_write(info, rec) == 0, "insert row 300");

  /* Delete middle row */
  {
    uchar key[4];
    int4store(key, 200);
    heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT);
    heap_delete(info, rec);
  }

  heap_scan_init(info);
  while ((error= heap_scan(info, scan_buf)) != HA_ERR_END_OF_FILE)
  {
    if (error == HA_ERR_RECORD_DELETED)
      deleted_count++;
    else if (error == 0)
      row_count++;
    else
      break;
  }

  ok(row_count == 2, "no-blob scan returned 2 live rows (got %u)", row_count);
  ok(deleted_count > 0,
     "no-blob scan returned HA_ERR_RECORD_DELETED for deleted slots (%u)",
     deleted_count);

  heap_drop_table(info);
  heap_close(info);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_scan");
  plan(19);

  diag("Test 1: scan skips continuation records internally");
  test_scan_skips_continuations();

  diag("Test 2: deleted rows + continuations");
  test_scan_deleted_plus_continuations();

  diag("Test 3: non-blob table scan unchanged");
  test_scan_no_blobs();

  my_end(0);
  return exit_status();
}
