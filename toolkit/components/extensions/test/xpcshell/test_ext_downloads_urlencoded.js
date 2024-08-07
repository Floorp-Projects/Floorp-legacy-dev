/* -*- Mode: indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set sts=2 sw=2 et tw=80: */
"use strict";

const { Downloads } = ChromeUtils.importESModule(
  "resource://gre/modules/Downloads.sys.mjs"
);

function backgroundScript() {
  let complete = new Map();

  function waitForComplete(id) {
    if (complete.has(id)) {
      return complete.get(id).promise;
    }

    let promise = new Promise(resolve => {
      complete.set(id, { resolve });
    });
    complete.get(id).promise = promise;
    return promise;
  }

  browser.downloads.onChanged.addListener(change => {
    if (change.state && change.state.current == "complete") {
      // Make sure we have a promise.
      waitForComplete(change.id);
      complete.get(change.id).resolve();
    }
  });

  browser.test.onMessage.addListener(async (msg, ...args) => {
    if (msg == "download.request") {
      try {
        let id = await browser.downloads.download(args[0]);
        browser.test.sendMessage("download.done", { status: "success", id });
      } catch (error) {
        browser.test.log(`downloads.download failed: ${error}`);
        browser.test.sendMessage("download.done", {
          status: "error",
          errmsg: error.message,
        });
      }
    } else if (msg == "search.request") {
      try {
        let downloads = await browser.downloads.search(args[0]);
        browser.test.sendMessage("search.done", {
          status: "success",
          downloads,
        });
      } catch (error) {
        browser.test.log(`downloads.search failed: ${error}`);
        browser.test.sendMessage("search.done", {
          status: "error",
          errmsg: error.message,
        });
      }
    } else if (msg == "waitForComplete.request") {
      await waitForComplete(args[0]);
      browser.test.sendMessage("waitForComplete.done");
    }
  });

  browser.test.sendMessage("ready");
}

async function clearDownloads(callback) {
  let list = await Downloads.getList(Downloads.ALL);
  let downloads = await list.getAll();

  await Promise.all(downloads.map(download => list.remove(download)));

  return downloads;
}

add_task(async function test_decoded_filename_download() {
  const server = createHttpServer();
  server.registerPrefixHandler("/data/", (_, res) => res.write("length=8"));

  const BASE = `http://localhost:${server.identity.primaryPort}/data`;
  const FILE_NAME_ENCODED_1 = "file%2Fencode.txt";
  const FILE_NAME_DECODED_1 = "file_encode.txt";
  const FILE_NAME_ENCODED_URL_1 = BASE + "/" + FILE_NAME_ENCODED_1;
  const FILE_NAME_ENCODED_2 = "file%F0%9F%9A%B2encoded.txt";
  const FILE_NAME_DECODED_2 = "file\u{0001F6B2}encoded.txt";
  const FILE_NAME_ENCODED_URL_2 = BASE + "/" + FILE_NAME_ENCODED_2;
  const FILE_NAME_ENCODED_3 = "file%X%20encode.txt";
  const FILE_NAME_DECODED_3 = "file_X encode.txt";
  const FILE_NAME_ENCODED_URL_3 = BASE + "/" + FILE_NAME_ENCODED_3;
  const FILE_NAME_ENCODED_4 = "file%E3%80%82encode.txt";
  const FILE_NAME_DECODED_4 = "file\u3002encode.txt";
  const FILE_NAME_ENCODED_URL_4 = BASE + "/" + FILE_NAME_ENCODED_4;
  const FILE_NAME_RAW_5 = "file%%percent%2.txt";
  const FILE_NAME_DECODED_5 = "file__percent_2.txt";
  const FILE_NAME_ENCODED_URL_5 = BASE + "/ignored-because-filename-param.txt";
  const FILE_ENCODED_LEN = 8;

  const nsIFile = Ci.nsIFile;
  let downloadDir = FileUtils.getDir("TmpD", ["downloads"]);
  downloadDir.createUnique(nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);
  info(`downloadDir ${downloadDir.path}`);

  function downloadPath(filename) {
    let path = downloadDir.clone();
    path.append(filename);
    return path.path;
  }

  Services.prefs.setIntPref("browser.download.folderList", 2);
  Services.prefs.setComplexValue("browser.download.dir", nsIFile, downloadDir);

  registerCleanupFunction(async () => {
    Services.prefs.clearUserPref("browser.download.folderList");
    Services.prefs.clearUserPref("browser.download.dir");
    await cleanupDir(downloadDir);
    await clearDownloads();
  });

  await clearDownloads().then(downloads => {
    info(`removed ${downloads.length} pre-existing downloads from history`);
  });

  let extension = ExtensionTestUtils.loadExtension({
    background: backgroundScript,
    manifest: {
      permissions: ["downloads"],
    },
  });

  async function download(options) {
    extension.sendMessage("download.request", options);
    let result = await extension.awaitMessage("download.done");

    if (result.status == "success") {
      info(`wait for onChanged event to indicate ${result.id} is complete`);
      extension.sendMessage("waitForComplete.request", result.id);

      await extension.awaitMessage("waitForComplete.done");
    }

    return result;
  }

  function search(query) {
    extension.sendMessage("search.request", query);
    return extension.awaitMessage("search.done");
  }

  await extension.startup();
  await extension.awaitMessage("ready");

  let downloadIds = {};
  let msg = await download({ url: FILE_NAME_ENCODED_URL_1 });
  equal(msg.status, "success", "download() succeeded");
  downloadIds.fileEncoded1 = msg.id;

  msg = await download({ url: FILE_NAME_ENCODED_URL_2 });
  equal(msg.status, "success", "download() succeeded");
  downloadIds.fileEncoded2 = msg.id;

  msg = await download({ url: FILE_NAME_ENCODED_URL_3 });
  equal(msg.status, "success", "download() succeeded");
  downloadIds.fileEncoded3 = msg.id;

  msg = await download({ url: FILE_NAME_ENCODED_URL_4 });
  equal(msg.status, "success", "download() succeeded");
  downloadIds.fileEncoded4 = msg.id;

  msg = await download({
    url: FILE_NAME_ENCODED_URL_5,
    filename: FILE_NAME_RAW_5,
  });
  equal(msg.status, "success", "download() succeeded");
  downloadIds.fileEncoded5 = msg.id;

  // Search for each individual download and check
  // the corresponding DownloadItem.
  async function checkDownloadItem(id, expect) {
    let item = await search({ id });
    equal(item.status, "success", "search() succeeded");
    equal(item.downloads.length, 1, "search() found exactly 1 download");
    Object.keys(expect).forEach(function (field) {
      equal(
        item.downloads[0][field],
        expect[field],
        `DownloadItem.${field} is correct"`
      );
    });
  }

  await checkDownloadItem(downloadIds.fileEncoded1, {
    url: FILE_NAME_ENCODED_URL_1,
    filename: downloadPath(FILE_NAME_DECODED_1),
    state: "complete",
    bytesReceived: FILE_ENCODED_LEN,
    totalBytes: FILE_ENCODED_LEN,
    fileSize: FILE_ENCODED_LEN,
    exists: true,
  });

  await checkDownloadItem(downloadIds.fileEncoded2, {
    url: FILE_NAME_ENCODED_URL_2,
    filename: downloadPath(FILE_NAME_DECODED_2),
    state: "complete",
    bytesReceived: FILE_ENCODED_LEN,
    totalBytes: FILE_ENCODED_LEN,
    fileSize: FILE_ENCODED_LEN,
    exists: true,
  });

  await checkDownloadItem(downloadIds.fileEncoded3, {
    url: FILE_NAME_ENCODED_URL_3,
    filename: downloadPath(FILE_NAME_DECODED_3),
    state: "complete",
    bytesReceived: FILE_ENCODED_LEN,
    totalBytes: FILE_ENCODED_LEN,
    fileSize: FILE_ENCODED_LEN,
    exists: true,
  });

  await checkDownloadItem(downloadIds.fileEncoded4, {
    url: FILE_NAME_ENCODED_URL_4,
    filename: downloadPath(FILE_NAME_DECODED_4),
    state: "complete",
    bytesReceived: FILE_ENCODED_LEN,
    totalBytes: FILE_ENCODED_LEN,
    fileSize: FILE_ENCODED_LEN,
    exists: true,
  });

  await checkDownloadItem(downloadIds.fileEncoded5, {
    url: FILE_NAME_ENCODED_URL_5,
    filename: downloadPath(FILE_NAME_DECODED_5),
    state: "complete",
    bytesReceived: FILE_ENCODED_LEN,
    totalBytes: FILE_ENCODED_LEN,
    fileSize: FILE_ENCODED_LEN,
    exists: true,
  });

  // Searching for downloads by the decoded filename works correctly.
  async function checkSearch(query, expected, description) {
    let item = await search(query);
    equal(item.status, "success", "search() succeeded");
    equal(
      item.downloads.length,
      expected.length,
      `search() for ${description} found exactly ${expected.length} downloads`
    );
    equal(
      item.downloads[0].id,
      downloadIds[expected[0]],
      `search() for ${description} returned ${expected[0]} in position ${0}`
    );
  }

  await checkSearch(
    { filename: downloadPath(FILE_NAME_DECODED_1) },
    ["fileEncoded1"],
    "filename"
  );
  await checkSearch(
    { filename: downloadPath(FILE_NAME_DECODED_2) },
    ["fileEncoded2"],
    "filename"
  );
  await checkSearch(
    { filename: downloadPath(FILE_NAME_DECODED_3) },
    ["fileEncoded3"],
    "filename"
  );
  await checkSearch(
    { filename: downloadPath(FILE_NAME_DECODED_4) },
    ["fileEncoded4"],
    "filename"
  );
  await checkSearch(
    { filename: downloadPath(FILE_NAME_DECODED_5) },
    ["fileEncoded5"],
    "filename"
  );

  await extension.unload();
});
