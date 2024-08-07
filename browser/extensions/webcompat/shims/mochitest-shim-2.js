/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/* globals browser */

if (!window.testPromise) {
  const originalUrl =
    "http://example.com/browser/browser/extensions/webcompat/tests/browser/shims_test_2.js";

  const shimId = "MochitestShim2";

  const sendMessageToAddon = (function () {
    const pendingMessages = new Map();
    const channel = new MessageChannel();
    channel.port1.onerror = console.error;
    channel.port1.onmessage = event => {
      const { messageId, response } = event.data;
      const resolve = pendingMessages.get(messageId);
      if (resolve) {
        pendingMessages.delete(messageId);
        resolve(response);
      }
    };
    function reconnect() {
      const detail = {
        pendingMessages: [...pendingMessages.values()],
        port: channel.port2,
        shimId,
      };
      window.dispatchEvent(new CustomEvent("ShimConnects", { detail }));
    }
    window.addEventListener("ShimHelperReady", reconnect);
    reconnect();
    return function (message) {
      const messageId =
        Math.random().toString(36).substring(2) + Date.now().toString(36);
      return new Promise(resolve => {
        const payload = {
          message,
          messageId,
          shimId,
        };
        pendingMessages.set(messageId, resolve);
        channel.port1.postMessage(payload);
      });
    };
  })();

  async function go(options) {
    try {
      const o = document.getElementById("shims");
      const cl = o.classList;
      cl.remove("red");
      cl.add("green");
      o.innerText = JSON.stringify(options || "");
    } catch (_) {}

    window.shimPromiseResolve("shimmed");

    if (window !== top) {
      window.optInPromiseResolve(false);
      return;
    }

    await sendMessageToAddon("optIn");

    window.doingOptIn = true;
    const s = document.createElement("script");
    s.src = originalUrl;
    s.onerror = () => window.optInPromiseResolve("error");
    document.head.appendChild(s);
  }

  sendMessageToAddon("getOptions").then(options => {
    if (document.readyState !== "loading") {
      go(options);
    } else {
      window.addEventListener("DOMContentLoaded", () => {
        go(options);
      });
    }
  });
}
