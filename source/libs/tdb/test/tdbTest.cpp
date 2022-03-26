#include "gtest/gtest.h"

#include "tdbInt.h"

#include <string>

static int tKeyCmpr(const void *pKey1, int kLen1, const void *pKey2, int kLen2);
static int tDefaultKeyCmpr(const void *pKey1, int keyLen1, const void *pKey2, int keyLen2);

TEST(tdb_test, simple_test) {
  int            ret;
  STEnv         *pEnv;
  STDB          *pDb;
  FKeyComparator compFunc;
  int            nData = 10000000;

  // Open Env
  ret = tdbEnvOpen("tdb", 4096, 256000, &pEnv);
  GTEST_ASSERT_EQ(ret, 0);

  // Create a database
  compFunc = tKeyCmpr;
  ret = tdbDbOpen("db.db", TDB_VARIANT_LEN, TDB_VARIANT_LEN, compFunc, pEnv, &pDb);
  GTEST_ASSERT_EQ(ret, 0);

  {
    char key[64];
    char val[64];

    {  // Insert some data

      for (int i = 1; i <= nData; i++) {
        sprintf(key, "key%d", i);
        sprintf(val, "value%d", i);
        ret = tdbDbInsert(pDb, key, strlen(key), val, strlen(val));
        GTEST_ASSERT_EQ(ret, 0);
      }
    }

    {  // Query the data
      void *pVal = NULL;
      int   vLen;

      for (int i = 1; i <= nData; i++) {
        sprintf(key, "key%d", i);
        sprintf(val, "value%d", i);

        ret = tdbDbGet(pDb, key, strlen(key), &pVal, &vLen);
        GTEST_ASSERT_EQ(ret, 0);

        GTEST_ASSERT_EQ(vLen, strlen(val));
        GTEST_ASSERT_EQ(memcmp(val, pVal, vLen), 0);
      }

      TDB_FREE(pVal);
    }

    {  // Iterate to query the DB data
      STDBC *pDBC;
      void  *pKey = NULL;
      void  *pVal = NULL;
      int    vLen, kLen;
      int    count = 0;

      ret = tdbDbcOpen(pDb, &pDBC);
      GTEST_ASSERT_EQ(ret, 0);

      for (;;) {
        ret = tdbDbNext(pDBC, &pKey, &kLen, &pVal, &vLen);
        if (ret < 0) break;

        // std::cout.write((char *)pKey, kLen) /* << " " << kLen */ << " ";
        // std::cout.write((char *)pVal, vLen) /* << " " << vLen */;
        // std::cout << std::endl;

        count++;
      }

      GTEST_ASSERT_EQ(count, nData);

      tdbDbcClose(pDBC);

      TDB_FREE(pKey);
      TDB_FREE(pVal);
    }
  }

  ret = tdbDbDrop(pDb);
  GTEST_ASSERT_EQ(ret, 0);

  // Close a database
  tdbDbClose(pDb);

  // Close Env
  ret = tdbEnvClose(pEnv);
  GTEST_ASSERT_EQ(ret, 0);
}

static int tKeyCmpr(const void *pKey1, int kLen1, const void *pKey2, int kLen2) {
  int k1, k2;

  std::string s1((char *)pKey1 + 3, kLen1 - 3);
  std::string s2((char *)pKey2 + 3, kLen2 - 3);
  k1 = stoi(s1);
  k2 = stoi(s2);

  if (k1 < k2) {
    return -1;
  } else if (k1 > k2) {
    return 1;
  } else {
    return 0;
  }
}

static int tDefaultKeyCmpr(const void *pKey1, int keyLen1, const void *pKey2, int keyLen2) {
  int mlen;
  int cret;

  ASSERT(keyLen1 > 0 && keyLen2 > 0 && pKey1 != NULL && pKey2 != NULL);

  mlen = keyLen1 < keyLen2 ? keyLen1 : keyLen2;
  cret = memcmp(pKey1, pKey2, mlen);
  if (cret == 0) {
    if (keyLen1 < keyLen2) {
      cret = -1;
    } else if (keyLen1 > keyLen2) {
      cret = 1;
    } else {
      cret = 0;
    }
  }
  return cret;
}