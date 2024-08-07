/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  DownloadPaths: "resource://gre/modules/DownloadPaths.sys.mjs",
  DownloadTracker: "resource://gre/modules/GeckoViewWebExtension.sys.mjs",
});

Cu.importGlobalProperties(["PathUtils"]);

var { ignoreEvent } = ExtensionCommon;

const REQUEST_DOWNLOAD_MESSAGE = "GeckoView:WebExtension:Download";

const FORBIDDEN_HEADERS = [
  "ACCEPT-CHARSET",
  "ACCEPT-ENCODING",
  "ACCESS-CONTROL-REQUEST-HEADERS",
  "ACCESS-CONTROL-REQUEST-METHOD",
  "CONNECTION",
  "CONTENT-LENGTH",
  "COOKIE",
  "COOKIE2",
  "DATE",
  "DNT",
  "EXPECT",
  "HOST",
  "KEEP-ALIVE",
  "ORIGIN",
  "TE",
  "TRAILER",
  "TRANSFER-ENCODING",
  "UPGRADE",
  "VIA",
];

const FORBIDDEN_PREFIXES = /^PROXY-|^SEC-/i;

const State = {
  IN_PROGRESS: "in_progress",
  INTERRUPTED: "interrupted",
  COMPLETE: "complete",
};

const STATE_MAP = new Map([
  [0, State.IN_PROGRESS],
  [1, State.INTERRUPTED],
  [2, State.COMPLETE],
]);

const INTERRUPT_REASON_MAP = new Map([
  [0, undefined],
  [1, "FILE_FAILED"],
  [2, "FILE_ACCESS_DENIED"],
  [3, "FILE_NO_SPACE"],
  [4, "FILE_NAME_TOO_LONG"],
  [5, "FILE_TOO_LARGE"],
  [6, "FILE_VIRUS_INFECTED"],
  [7, "FILE_TRANSIENT_ERROR"],
  [8, "FILE_BLOCKED"],
  [9, "FILE_SECURITY_CHECK_FAILED"],
  [10, "FILE_TOO_SHORT"],
  [11, "NETWORK_FAILED"],
  [12, "NETWORK_TIMEOUT"],
  [13, "NETWORK_DISCONNECTED"],
  [14, "NETWORK_SERVER_DOWN"],
  [15, "NETWORK_INVALID_REQUEST"],
  [16, "SERVER_FAILED"],
  [17, "SERVER_NO_RANGE"],
  [18, "SERVER_BAD_CONTENT"],
  [19, "SERVER_UNAUTHORIZED"],
  [20, "SERVER_CERT_PROBLEM"],
  [21, "SERVER_FORBIDDEN"],
  [22, "USER_CANCELED"],
  [23, "USER_SHUTDOWN"],
  [24, "CRASH"],
]);

// TODO Bug 1247794: make id and extension info persistent
class DownloadItem {
  /**
   * Initializes an object that represents a download
   *
   * @param {object} downloadInfo - an object from Java when creating a download
   * @param {object} options - an object passed in to download() function
   * @param {Extension} extension - instance of an extension object
   */
  constructor(downloadInfo, options, extension) {
    this.id = downloadInfo.id;
    this.url = options.url;
    this.referrer = downloadInfo.referrer || "";
    this.filename = downloadInfo.filename || "";
    this.incognito = options.incognito;
    this.danger = "safe"; // todo; not implemented in desktop either
    this.mime = downloadInfo.mime || "";
    this.startTime = downloadInfo.startTime;
    this.state = STATE_MAP.get(downloadInfo.state);
    this.paused = downloadInfo.paused;
    this.canResume = downloadInfo.canResume;
    this.bytesReceived = downloadInfo.bytesReceived;
    this.totalBytes = downloadInfo.totalBytes;
    this.fileSize = downloadInfo.fileSize;
    this.exists = downloadInfo.exists;
    this.byExtensionId = extension?.id;
    this.byExtensionName = extension?.name;
  }

  /**
   * This function updates the download item it was called on.
   *
   * @param {object} data that arrived from the app (Java)
   * @returns {object | null} an object of <a href="https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/downloads/onChanged#downloaddelta>downloadDelta type</a>
   */
  update(data) {
    const { downloadItemId } = data;
    const delta = {};

    data.state = STATE_MAP.get(data.state);
    data.error = INTERRUPT_REASON_MAP.get(data.error);
    delete data.downloadItemId;

    let changed = false;
    for (const prop in data) {
      const current = data[prop] ?? null;
      const previous = this[prop] ?? null;
      if (current !== previous) {
        delta[prop] = { current, previous };
        this[prop] = current;
        changed = true;
      }
    }

    // Don't send empty onChange events
    if (!changed) {
      return null;
    }

    delta.id = downloadItemId;

    return delta;
  }
}

this.downloads = class extends ExtensionAPIPersistent {
  PERSISTENT_EVENTS = {
    onChanged({ fire, context }, params) {
      const listener = (eventName, event) => {
        const { delta, downloadItem } = event;
        const { extension } = this;
        if (extension.privateBrowsingAllowed || !downloadItem.incognito) {
          fire.async(delta);
        }
      };
      DownloadTracker.on("download-changed", listener);

      return {
        unregister() {
          DownloadTracker.off("download-changed", listener);
        },
        convert(_fire, _context) {
          fire = _fire;
          context = _context;
        },
      };
    },
  };

  getAPI(context) {
    const { extension } = context;
    return {
      downloads: {
        download(options) {
          // the validation checks should be kept in sync with the toolkit implementation
          let { filename } = options;
          if (filename != null) {
            if (!filename.length) {
              return Promise.reject({ message: "filename must not be empty" });
            }

            if (PathUtils.isAbsolute(filename)) {
              return Promise.reject({
                message: "filename must not be an absolute path",
              });
            }

            // % is not permitted but relatively common.
            filename = filename.replaceAll("%", "_");

            const pathComponents = PathUtils.splitRelative(filename, {
              allowEmpty: true,
              allowCurrentDir: true,
              allowParentDir: true,
            });

            if (pathComponents.some(component => component == "..")) {
              return Promise.reject({
                message: "filename must not contain back-references (..)",
              });
            }

            if (
              pathComponents.some(component => {
                const sanitized = DownloadPaths.sanitize(component, {
                  compressWhitespaces: false,
                });
                return component != sanitized;
              })
            ) {
              return Promise.reject({
                message: "filename must not contain illegal characters",
              });
            }
          }

          if (options.incognito && !context.privateBrowsingAllowed) {
            return Promise.reject({
              message: "Private browsing access not allowed",
            });
          }

          if (options.cookieStoreId != null) {
            // https://bugzilla.mozilla.org/show_bug.cgi?id=1721460
            throw new ExtensionError("Not implemented");
          }

          if (options.headers) {
            for (const { name } of options.headers) {
              if (
                FORBIDDEN_HEADERS.includes(name.toUpperCase()) ||
                name.match(FORBIDDEN_PREFIXES)
              ) {
                return Promise.reject({
                  message: "Forbidden request header name",
                });
              }
            }
          }

          return EventDispatcher.instance
            .sendRequestForResult({
              type: REQUEST_DOWNLOAD_MESSAGE,
              options,
              extensionId: extension.id,
            })
            .then(value => {
              const downloadItem = new DownloadItem(value, options, extension);
              DownloadTracker.addDownloadItem(downloadItem);
              return downloadItem.id;
            });
        },

        removeFile(downloadId) {
          throw new ExtensionError("Not implemented");
        },

        search(query) {
          throw new ExtensionError("Not implemented");
        },

        pause(downloadId) {
          throw new ExtensionError("Not implemented");
        },

        resume(downloadId) {
          throw new ExtensionError("Not implemented");
        },

        cancel(downloadId) {
          throw new ExtensionError("Not implemented");
        },

        showDefaultFolder() {
          throw new ExtensionError("Not implemented");
        },

        erase(query) {
          throw new ExtensionError("Not implemented");
        },

        open(downloadId) {
          throw new ExtensionError("Not implemented");
        },

        show(downloadId) {
          throw new ExtensionError("Not implemented");
        },

        getFileIcon(downloadId, options) {
          throw new ExtensionError("Not implemented");
        },

        onChanged: new EventManager({
          context,
          module: "downloads",
          event: "onChanged",
          extensionApi: this,
        }).api(),

        onCreated: ignoreEvent(context, "downloads.onCreated"),

        onErased: ignoreEvent(context, "downloads.onErased"),

        onDeterminingFilename: ignoreEvent(
          context,
          "downloads.onDeterminingFilename"
        ),
      },
    };
  }
};
