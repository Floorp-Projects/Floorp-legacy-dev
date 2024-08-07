/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

if (!window.ramblerIdHelper) {
  const originalScript = (() => {
    const src = document.currentScript?.src;
    try {
      const { protocol, hostname, pathname, href } = new URL(src);
      if (
        (protocol === "http:" || protocol === "https:") &&
        hostname === "id.rambler.ru" &&
        pathname === "rambler-id-helper/auth_events.js"
      ) {
        return href;
      }
    } catch (_) {}
    return "https://id.rambler.ru/rambler-id-helper/auth_events.js";
  })();

  const sendMessageToAddon = (function () {
    const shimId = "Rambler";
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

  const ramblerIdHelper = {
    getProfileInfo: (successCallback, errorCallback) => {
      successCallback({});
    },
    openAuth: () => {
      sendMessageToAddon("optIn").then(function () {
        const openAuthArgs = arguments;
        window.ramblerIdHelper = undefined;
        const s = document.createElement("script");
        s.src = originalScript;
        document.head.appendChild(s);
        s.addEventListener("load", () => {
          const helper = window.ramblerIdHelper;
          for (const { fn, args } of callLog) {
            helper[fn].apply(helper, args);
          }
          helper.openAuth.apply(helper, openAuthArgs);
        });
      });
    },
  };

  const callLog = [];
  function addLoggedCall(fn) {
    ramblerIdHelper[fn] = () => {
      callLog.push({ fn, args: arguments });
    };
  }

  addLoggedCall("registerOnFrameCloseCallback");
  addLoggedCall("registerOnFrameRedirect");
  addLoggedCall("registerOnPossibleLoginCallback");
  addLoggedCall("registerOnPossibleLogoutCallback");
  addLoggedCall("registerOnPossibleOauthLoginCallback");

  window.ramblerIdHelper = ramblerIdHelper;
}
