/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * Bug 1226498 - Shim Facebook SDK
 *
 * This shim provides functionality to enable Facebook's authenticator on third
 * party sites ("continue/log in with Facebook" buttons). This includes rendering
 * the button as the SDK would, if sites require it. This way, if users wish to
 * opt into the Facebook login process regardless of the tracking consequences,
 * they only need to click the button as usual.
 *
 * In addition, the shim also attempts to provide placeholders for Facebook
 * videos, which users may click to opt into seeing the video (also despite
 * the increased tracking risks). This is an experimental feature enabled
 * that is only currently enabled on nightly builds.
 *
 * Finally, this shim also stubs out as much of the SDK as possible to prevent
 * breaking on sites which expect that it will always successfully load.
 */

if (!window.FB) {
  const FacebookLogoURL = "https://smartblock.firefox.etp/facebook.svg";
  const PlayIconURL = "https://smartblock.firefox.etp/play.svg";

  const originalUrl = (() => {
    const src = document.currentScript?.src;
    try {
      const { protocol, hostname, pathname, href } = new URL(src);
      if (
        (protocol === "http:" || protocol === "https:") &&
        hostname === "connect.facebook.net" &&
        (pathname.endsWith("/sdk.js") || pathname.endsWith("/all.js"))
      ) {
        return href;
      }
      if (href.includes("all.js")) {
        // Legacy SDK.
        return "https://connect.facebook.net/en_US/all.js";
      }
    } catch (_) {}
    return "https://connect.facebook.net/en_US/sdk.js";
  })();

  let haveUnshimmed;
  let initInfo;
  let activeOnloginAttribute;
  const placeholdersToRemoveOnUnshim = new Set();
  const loggedGraphApiCalls = [];
  const eventHandlers = new Map();

  function getGUID() {
    const v = crypto.getRandomValues(new Uint8Array(20));
    return Array.from(v, c => c.toString(16)).join("");
  }

  const sendMessageToAddon = (function () {
    const shimId = "FacebookSDK";
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
      const messageId = getGUID();
      return new Promise(resolve => {
        const payload = { message, messageId, shimId };
        pendingMessages.set(messageId, resolve);
        channel.port1.postMessage(payload);
      });
    };
  })();

  const isNightly = sendMessageToAddon("getOptions").then(opts => {
    return opts.releaseBranch === "nightly";
  });

  function makeLoginPlaceholder(target) {
    // Sites may provide their own login buttons, or rely on the Facebook SDK
    // to render one for them. For the latter case, we provide placeholders
    // which try to match the examples and documentation here:
    // https://developers.facebook.com/docs/facebook-login/web/login-button/

    if (target.textContent || target.hasAttribute("fb-xfbml-state")) {
      return;
    }
    target.setAttribute("fb-xfbml-state", "");

    const size = target.getAttribute("data-size") || "large";

    let font, margin, minWidth, maxWidth, height, iconHeight;
    if (size === "small") {
      font = 11;
      margin = 8;
      minWidth = maxWidth = 200;
      height = 20;
      iconHeight = 12;
    } else if (size === "medium") {
      font = 13;
      margin = 8;
      minWidth = 200;
      maxWidth = 320;
      height = 28;
      iconHeight = 16;
    } else {
      font = 16;
      minWidth = 240;
      maxWidth = 400;
      margin = 12;
      height = 40;
      iconHeight = 24;
    }

    const wattr = target.getAttribute("data-width") || "";
    const width =
      wattr === "100%" ? wattr : `${parseFloat(wattr) || minWidth}px`;

    const round = target.getAttribute("data-layout") === "rounded" ? 20 : 4;

    const text =
      target.getAttribute("data-button-type") === "continue_with"
        ? "Continue with Facebook"
        : "Log in with Facebook";

    const button = document.createElement("div");
    button.style = `
      display: flex;
      align-items: center;
      justify-content: center;
      padding-left: ${margin + iconHeight}px;
      ${width};
      min-width: ${minWidth}px;
      max-width: ${maxWidth}px;
      height: ${height}px;
      border-radius: ${round}px;
      -moz-text-size-adjust: none;
      -moz-user-select: none;
      color: #fff;
      font-size: ${font}px;
      font-weight: bold;
      font-family: Helvetica, Arial, sans-serif;
      letter-spacing: .25px;
      background-color: #1877f2;
      background-repeat: no-repeat;
      background-position: ${margin}px 50%;
      background-size: ${iconHeight}px ${iconHeight}px;
      background-image: url(${FacebookLogoURL});
    `;
    button.textContent = text;
    target.appendChild(button);
    target.addEventListener("click", () => {
      activeOnloginAttribute = target.getAttribute("onlogin");
    });
  }

  async function makeVideoPlaceholder(target) {
    // For videos, we provide a more generic placeholder of roughly the
    // expected size with a play button, as well as a Facebook logo.
    if (!(await isNightly) || target.hasAttribute("fb-xfbml-state")) {
      return;
    }
    target.setAttribute("fb-xfbml-state", "");

    let width = parseInt(target.getAttribute("data-width"));
    let height = parseInt(target.getAttribute("data-height"));
    if (height) {
      height = `${width * 0.6}px`;
    } else {
      height = `100%; min-height:${width * 0.75}px`;
    }
    if (width) {
      width = `${width}px`;
    } else {
      width = `100%; min-width:200px`;
    }

    const placeholder = document.createElement("div");
    placeholdersToRemoveOnUnshim.add(placeholder);
    placeholder.style = `
      width: ${width};
      height: ${height};
      top: 0px;
      left: 0px;
      background: #000;
      color: #fff;
      text-align: center;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      background-image: url(${FacebookLogoURL}), url(${PlayIconURL});
      background-position: calc(100% - 24px) 24px, 50% 47.5%;
      background-repeat: no-repeat, no-repeat;
      background-size: 43px 42px, 25% 25%;
      -moz-text-size-adjust: none;
      -moz-user-select: none;
      color: #fff;
      align-items: center;
      padding-top: 200px;
      font-size: 14pt;
    `;
    placeholder.textContent = "Click to allow blocked Facebook content";
    placeholder.addEventListener("click", evt => {
      if (!evt.isTrusted) {
        return;
      }
      allowFacebookSDK(() => {
        placeholdersToRemoveOnUnshim.forEach(p => p.remove());
      });
    });

    target.innerHTML = "";
    target.appendChild(placeholder);
  }

  // We monitor for XFBML objects as Facebook SDK does, so we
  // can provide placeholders for dynamically-added ones.
  const xfbmlObserver = new MutationObserver(mutations => {
    for (let { addedNodes, target, type } of mutations) {
      const nodes = type === "attributes" ? [target] : addedNodes;
      for (const node of nodes) {
        if (node?.classList?.contains("fb-login-button")) {
          makeLoginPlaceholder(node);
        }
        if (node?.classList?.contains("fb-video")) {
          makeVideoPlaceholder(node);
        }
      }
    }
  });

  xfbmlObserver.observe(document.documentElement, {
    childList: true,
    subtree: true,
    attributes: true,
    attributeFilter: ["class"],
  });

  const needPopup =
    !/app_runner/.test(window.name) && !/iframe_canvas/.test(window.name);
  const popupName = getGUID();
  let activePopup;

  if (needPopup) {
    const oldWindowOpen = window.open;
    window.open = function (href, name, params) {
      try {
        const url = new URL(href, window.location.href);
        if (
          url.protocol === "https:" &&
          (url.hostname === "m.facebook.com" ||
            url.hostname === "www.facebook.com") &&
          url.pathname.endsWith("/oauth")
        ) {
          name = popupName;
        }
      } catch (e) {
        console.error(e);
      }
      return oldWindowOpen.call(window, href, name, params);
    };
  }

  let allowingFacebookPromise;

  async function allowFacebookSDK(postInitCallback) {
    if (allowingFacebookPromise) {
      return allowingFacebookPromise;
    }

    let resolve, reject;
    allowingFacebookPromise = new Promise((_resolve, _reject) => {
      resolve = _resolve;
      reject = _reject;
    });

    await sendMessageToAddon("optIn");

    xfbmlObserver.disconnect();

    const shim = window.FB;
    window.FB = undefined;

    // We need to pass the site's initialization info to the real
    // SDK as it loads, so we use the fbAsyncInit mechanism to
    // do so, also ensuring our own post-init callbacks are called.
    const oldInit = window.fbAsyncInit;
    window.fbAsyncInit = () => {
      try {
        if (typeof initInfo !== "undefined") {
          window.FB.init(initInfo);
        } else if (oldInit) {
          oldInit();
        }
      } catch (e) {
        console.error(e);
      }

      // Also re-subscribe any SDK event listeners as early as possible.
      for (const [name, fns] of eventHandlers.entries()) {
        for (const fn of fns) {
          window.FB.Event.subscribe(name, fn);
        }
      }

      // Allow the shim to do any post-init work early as well, while the
      // SDK script finishes loading and we ask it to re-parse XFBML etc.
      postInitCallback?.();
    };

    const script = document.createElement("script");
    script.src = originalUrl;

    script.addEventListener("error", () => {
      allowingFacebookPromise = null;
      script.remove();
      activePopup?.close();
      window.FB = shim;
      reject();
      alert("Failed to load Facebook SDK; please try again");
    });

    script.addEventListener("load", () => {
      haveUnshimmed = true;

      // After the real SDK has fully loaded we re-issue any Graph API
      // calls the page is waiting on, as well as requesting for it to
      // re-parse any XBFML elements (including ones with placeholders).

      for (const args of loggedGraphApiCalls) {
        try {
          window.FB.api.apply(window.FB, args);
        } catch (e) {
          console.error(e);
        }
      }

      window.FB.XFBML.parse(document.body, resolve);
    });

    document.head.appendChild(script);

    return allowingFacebookPromise;
  }

  function buildPopupParams() {
    // We try to match Facebook's popup size reasonably closely.
    const { outerWidth, outerHeight, screenX, screenY } = window;
    const { width, height } = window.screen;
    const w = Math.min(width, 400);
    const h = Math.min(height, 400);
    const ua = navigator.userAgent;
    const isMobile = ua.includes("Mobile") || ua.includes("Tablet");
    const left = screenX + (screenX < 0 ? width : 0) + (outerWidth - w) / 2;
    const top = screenY + (screenY < 0 ? height : 0) + (outerHeight - h) / 2.5;
    let params = `left=${left},top=${top},width=${w},height=${h},scrollbars=1,toolbar=0,location=1`;
    if (!isMobile) {
      params = `${params},width=${w},height=${h}`;
    }
    return params;
  }

  // If a page stores the window.FB reference of the shim, then we
  // want to have it proxy calls to the real SDK once we've unshimmed.
  function ensureProxiedToUnshimmed(obj) {
    const shim = {};
    for (const key in obj) {
      const value = obj[key];
      if (typeof value === "function") {
        shim[key] = function () {
          if (haveUnshimmed) {
            return window.FB[key].apply(window.FB, arguments);
          }
          return value.apply(this, arguments);
        };
      } else if (typeof value !== "object" || value === null) {
        shim[key] = value;
      } else {
        shim[key] = ensureProxiedToUnshimmed(value);
      }
    }
    return new Proxy(shim, {
      get: (shimmed, key) => (haveUnshimmed ? window.FB : shimmed)[key],
    });
  }

  window.FB = ensureProxiedToUnshimmed({
    api() {
      loggedGraphApiCalls.push(arguments);
    },
    AppEvents: {
      activateApp() {},
      clearAppVersion() {},
      clearUserID() {},
      EventNames: {
        ACHIEVED_LEVEL: "fb_mobile_level_achieved",
        ADDED_PAYMENT_INFO: "fb_mobile_add_payment_info",
        ADDED_TO_CART: "fb_mobile_add_to_cart",
        ADDED_TO_WISHLIST: "fb_mobile_add_to_wishlist",
        COMPLETED_REGISTRATION: "fb_mobile_complete_registration",
        COMPLETED_TUTORIAL: "fb_mobile_tutorial_completion",
        INITIATED_CHECKOUT: "fb_mobile_initiated_checkout",
        PAGE_VIEW: "fb_page_view",
        RATED: "fb_mobile_rate",
        SEARCHED: "fb_mobile_search",
        SPENT_CREDITS: "fb_mobile_spent_credits",
        UNLOCKED_ACHIEVEMENT: "fb_mobile_achievement_unlocked",
        VIEWED_CONTENT: "fb_mobile_content_view",
      },
      getAppVersion: () => "",
      getUserID: () => "",
      logEvent() {},
      logPageView() {},
      logPurchase() {},
      ParameterNames: {
        APP_USER_ID: "_app_user_id",
        APP_VERSION: "_appVersion",
        CONTENT_ID: "fb_content_id",
        CONTENT_TYPE: "fb_content_type",
        CURRENCY: "fb_currency",
        DESCRIPTION: "fb_description",
        LEVEL: "fb_level",
        MAX_RATING_VALUE: "fb_max_rating_value",
        NUM_ITEMS: "fb_num_items",
        PAYMENT_INFO_AVAILABLE: "fb_payment_info_available",
        REGISTRATION_METHOD: "fb_registration_method",
        SEARCH_STRING: "fb_search_string",
        SUCCESS: "fb_success",
      },
      setAppVersion() {},
      setUserID() {},
      updateUserProperties() {},
    },
    Canvas: {
      getHash: () => "",
      getPageInfo(cb) {
        cb?.call(this, {
          clientHeight: 1,
          clientWidth: 1,
          offsetLeft: 0,
          offsetTop: 0,
          scrollLeft: 0,
          scrollTop: 0,
        });
      },
      Plugin: {
        hidePluginElement() {},
        showPluginElement() {},
      },
      Prefetcher: {
        COLLECT_AUTOMATIC: 0,
        COLLECT_MANUAL: 1,
        addStaticResource() {},
        setCollectionMode() {},
      },
      scrollTo() {},
      setAutoGrow() {},
      setDoneLoading() {},
      setHash() {},
      setSize() {},
      setUrlHandler() {},
      startTimer() {},
      stopTimer() {},
    },
    Event: {
      subscribe(e, f) {
        if (!eventHandlers.has(e)) {
          eventHandlers.set(e, new Set());
        }
        eventHandlers.get(e).add(f);
      },
      unsubscribe(e, f) {
        eventHandlers.get(e)?.delete(f);
      },
    },
    frictionless: {
      init() {},
      isAllowed: () => false,
    },
    gamingservices: {
      friendFinder() {},
      uploadImageToMediaLibrary() {},
    },
    getAccessToken: () => null,
    getAuthResponse() {
      return { status: "" };
    },
    getLoginStatus(cb) {
      cb?.call(this, { status: "unknown" });
    },
    getUserID() {},
    init(_initInfo) {
      initInfo = _initInfo; // in case the site is not using fbAsyncInit
    },
    login(cb, opts) {
      // We have to load Facebook's script, and then wait for it to call
      // window.open. By that time, the popup blocker will likely trigger.
      // So we open a popup now with about:blank, and then make sure FB
      // will re-use that same popup later.
      if (needPopup) {
        activePopup = window.open("about:blank", popupName, buildPopupParams());
      }
      allowFacebookSDK(() => {
        activePopup = undefined;
        function runPostLoginCallbacks() {
          try {
            cb?.apply(this, arguments);
          } catch (e) {
            console.error(e);
          }
          if (activeOnloginAttribute) {
            setTimeout(activeOnloginAttribute, 1);
            activeOnloginAttribute = undefined;
          }
        }
        window.FB.login(runPostLoginCallbacks, opts);
      }).catch(() => {
        activePopup = undefined;
        activeOnloginAttribute = undefined;
        try {
          cb?.({});
        } catch (e) {
          console.error(e);
        }
      });
    },
    logout(cb) {
      cb?.call(this);
    },
    ui(params, fn) {
      if (params.method === "permissions.oauth") {
        window.FB.login(fn, params);
      }
    },
    XFBML: {
      parse(node, cb) {
        node = node || document;
        node.querySelectorAll(".fb-login-button").forEach(makeLoginPlaceholder);
        node.querySelectorAll(".fb-video").forEach(makeVideoPlaceholder);
        try {
          cb?.call(this);
        } catch (e) {
          console.error(e);
        }
      },
    },
  });

  window.FB.XFBML.parse();

  window?.fbAsyncInit?.();
}
