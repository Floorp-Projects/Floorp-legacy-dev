/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * Bug 1713710 - Shim Vidible video player
 *
 * Sites relying on Vidible's video player may experience broken videos if that
 * script is blocked. This shim allows users to opt into viewing those videos
 * regardless of any tracking consequences, by providing placeholders for each.
 */

if (!window.vidible?.version) {
  const PlayIconURL = "https://smartblock.firefox.etp/play.svg";

  const originalScript = (() => {
    const src = document.currentScript?.src;
    try {
      const { protocol, hostname, pathname, href } = new URL(src);
      if (
        (protocol === "http:" || protocol === "https:") &&
        pathname.endsWith("/vidible-min.js") &&
        (hostname.endsWith(".vidible.tv") ||
          hostname === "vdb-cdn-files.s3.amazonaws.com")
      ) {
        return href;
      }
    } catch (_) {}
    return "https://cdn-ssl.vidible.tv/prod/player/js/21.1.1/vidible-min.js";
  })();

  const getGUID = () => {
    const v = crypto.getRandomValues(new Uint8Array(20));
    return Array.from(v, c => c.toString(16)).join("");
  };

  const sendMessageToAddon = (function () {
    const shimId = "Vidible";
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

  const Shimmer = (function () {
    // If a page might store references to an object before we replace it,
    // ensure that it only receives proxies to that object created by
    // `Shimmer.proxy(obj)`. Later when the unshimmed object is created,
    // call `Shimmer.unshim(proxy, unshimmed)`. This way the references
    // will automatically "become" the unshimmed object when appropriate.

    const shimmedObjects = new WeakMap();
    const unshimmedObjects = new Map();

    function proxy(shim) {
      if (shimmedObjects.has(shim)) {
        return shimmedObjects.get(shim);
      }

      const prox = new Proxy(shim, {
        get: (target, k) => {
          if (unshimmedObjects.has(prox)) {
            return unshimmedObjects.get(prox)[k];
          }
          return target[k];
        },
        apply: (target, thisArg, args) => {
          if (unshimmedObjects.has(prox)) {
            return unshimmedObjects.get(prox)(...args);
          }
          return target.apply(thisArg, args);
        },
        construct: (target, args) => {
          if (unshimmedObjects.has(prox)) {
            return new unshimmedObjects.get(prox)(...args);
          }
          return new target(...args);
        },
      });
      shimmedObjects.set(shim, prox);
      shimmedObjects.set(prox, prox);

      for (const key in shim) {
        const value = shim[key];
        if (typeof value === "function") {
          shim[key] = function () {
            const unshimmed = unshimmedObjects.get(prox);
            if (unshimmed) {
              return unshimmed[key].apply(unshimmed, arguments);
            }
            return value.apply(this, arguments);
          };
        } else if (typeof value !== "object" || value === null) {
          shim[key] = value;
        } else {
          shim[key] = Shimmer.proxy(value);
        }
      }

      return prox;
    }

    function unshim(shim, unshimmed) {
      unshimmedObjects.set(shim, unshimmed);

      for (const prop in shim) {
        if (prop in unshimmed) {
          const un = unshimmed[prop];
          if (typeof un === "object" && un !== null) {
            unshim(shim[prop], un);
          }
        } else {
          unshimmedObjects.set(shim[prop], undefined);
        }
      }
    }

    return { proxy, unshim };
  })();

  const extras = [];
  const playersByNode = new WeakMap();
  const playerData = new Map();

  const getJSONPVideoPlacements = () => {
    return document.querySelectorAll(
      `script[src*="delivery.vidible.tv/jsonp"]`
    );
  };

  const allowVidible = () => {
    if (allowVidible.promise) {
      return allowVidible.promise;
    }

    const shim = window.vidible;
    window.vidible = undefined;

    allowVidible.promise = sendMessageToAddon("optIn")
      .then(() => {
        return new Promise((resolve, reject) => {
          const script = document.createElement("script");
          script.src = originalScript;
          script.addEventListener("load", () => {
            Shimmer.unshim(shim, window.vidible);

            for (const args of extras) {
              window.visible.registerExtra(...args);
            }

            for (const jsonp of getJSONPVideoPlacements()) {
              const { src } = jsonp;
              const jscript = document.createElement("script");
              jscript.onload = resolve;
              jscript.src = src;
              jsonp.replaceWith(jscript);
            }

            for (const [playerShim, data] of playerData.entries()) {
              const { loadCalled, on, parent, placeholder, setup } = data;

              placeholder?.remove();

              const player = window.vidible.player(parent);
              Shimmer.unshim(playerShim, player);

              for (const [type, fns] of on.entries()) {
                for (const fn of fns) {
                  try {
                    player.on(type, fn);
                  } catch (e) {
                    console.error(e);
                  }
                }
              }

              if (setup) {
                player.setup(setup);
              }

              if (loadCalled) {
                player.load();
              }
            }

            resolve();
          });

          script.addEventListener("error", () => {
            script.remove();
            reject();
          });

          document.head.appendChild(script);
        });
      })
      .catch(() => {
        window.vidible = shim;
        delete allowVidible.promise;
      });

    return allowVidible.promise;
  };

  const createVideoPlaceholder = (service, callback) => {
    const placeholder = document.createElement("div");
    placeholder.style = `
      position: absolute;
      width: 100%;
      height: 100%;
      min-width: 160px;
      min-height: 100px;
      top: 0px;
      left: 0px;
      background: #000;
      color: #fff;
      text-align: center;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      background-image: url(${PlayIconURL});
      background-position: 50% 47.5%;
      background-repeat: no-repeat;
      background-size: 25% 25%;
      -moz-text-size-adjust: none;
      -moz-user-select: none;
      color: #fff;
      align-items: center;
      padding-top: 200px;
      font-size: 14pt;
    `;
    placeholder.textContent = `Click to allow blocked ${service} content`;
    placeholder.addEventListener("click", evt => {
      evt.isTrusted && callback();
    });
    return placeholder;
  };

  const Player = function (parent) {
    const existing = playersByNode.get(parent);
    if (existing) {
      return existing;
    }

    const player = Shimmer.proxy(this);
    playersByNode.set(parent, player);

    const placeholder = createVideoPlaceholder("Vidible", allowVidible);
    parent.parentNode.insertBefore(placeholder, parent);

    playerData.set(player, {
      on: new Map(),
      parent,
      placeholder,
    });
    return player;
  };

  const changeData = function (fn) {
    const data = playerData.get(this);
    if (data) {
      fn(data);
      playerData.set(this, data);
    }
  };

  Player.prototype = {
    addEventListener() {},
    destroy() {
      const { placeholder } = playerData.get(this);
      placeholder?.remove();
      playerData.delete(this);
    },
    dispatchEvent() {},
    getAdsPassedTime() {},
    getAllMacros() {},
    getCurrentTime() {},
    getDuration() {},
    getHeight() {},
    getPixelsLog() {},
    getPlayerContainer() {},
    getPlayerInfo() {},
    getPlayerStatus() {},
    getRequestsLog() {},
    getStripUrl() {},
    getVolume() {},
    getWidth() {},
    hidePlayReplayControls() {},
    isMuted() {},
    isPlaying() {},
    load() {
      changeData(data => (data.loadCalled = true));
    },
    mute() {},
    on(type, fn) {
      changeData(({ on }) => {
        if (!on.has(type)) {
          on.set(type, new Set());
        }
        on.get(type).add(fn);
      });
    },
    off(type, fn) {
      changeData(({ on }) => {
        on.get(type)?.delete(fn);
      });
    },
    overrideMacro() {},
    pause() {},
    play() {},
    playVideoByIndex() {},
    removeEventListener() {},
    seekTo() {},
    sendBirthDate() {},
    sendKey() {},
    setup(s) {
      changeData(data => (data.setup = s));
      return this;
    },
    setVideosToPlay() {},
    setVolume() {},
    showPlayReplayControls() {},
    toggleFullscreen() {},
    toggleMute() {},
    togglePlay() {},
    updateBid() {},
    version() {},
    volume() {},
  };

  const vidible = {
    ADVERT_CLOSED: "advertClosed",
    AD_END: "adend",
    AD_META: "admeta",
    AD_PAUSED: "adpaused",
    AD_PLAY: "adplay",
    AD_START: "adstart",
    AD_TIMEUPDATE: "adtimeupdate",
    AD_WAITING: "adwaiting",
    AGE_GATE_DISPLAYED: "agegatedisplayed",
    BID_UPDATED: "BidUpdated",
    CAROUSEL_CLICK: "CarouselClick",
    CONTEXT_ENDED: "contextended",
    CONTEXT_STARTED: "contextstarted",
    ENTER_FULLSCREEN: "playerenterfullscreen",
    EXIT_FULLSCREEN: "playerexitfullscreen",
    FALLBACK: "fallback",
    FLOAT_END_ACTION: "floatended",
    FLOAT_START_ACTION: "floatstarted",
    HIDE_PLAY_REPLAY_BUTTON: "hideplayreplaybutton",
    LIGHTBOX_ACTIVATED: "lightboxactivated",
    LIGHTBOX_DEACTIVATED: "lightboxdeactivated",
    MUTE: "Mute",
    PLAYER_CONTROLS_STATE_CHANGE: "playercontrolsstatechaned",
    PLAYER_DOCKED: "playerDocked",
    PLAYER_ERROR: "playererror",
    PLAYER_FLOATING: "playerFloating",
    PLAYER_READY: "playerready",
    PLAYER_RESIZE: "playerresize",
    PLAYLIST_END: "playlistend",
    SEEK_END: "SeekEnd",
    SEEK_START: "SeekStart",
    SHARE_SCREEN_CLOSED: "sharescreenclosed",
    SHARE_SCREEN_OPENED: "sharescreenopened",
    SHOW_PLAY_REPLAY_BUTTON: "showplayreplaybutton",
    SUBTITLES_DISABLED: "subtitlesdisabled",
    SUBTITLES_ENABLED: "subtitlesenabled",
    SUBTITLES_READY: "subtitlesready",
    UNMUTE: "Unmute",
    VIDEO_DATA_LOADED: "videodataloaded",
    VIDEO_END: "videoend",
    VIDEO_META: "videometadata",
    VIDEO_MODULE_CREATED: "videomodulecreated",
    VIDEO_PAUSE: "videopause",
    VIDEO_PLAY: "videoplay",
    VIDEO_SEEKEND: "videoseekend",
    VIDEO_SELECTED: "videoselected",
    VIDEO_START: "videostart",
    VIDEO_TIMEUPDATE: "videotimeupdate",
    VIDEO_VOLUME_CHANGED: "videovolumechanged",
    VOLUME: "Volume",
    _getContexts: () => [],
    "content.CLICK": "content.click",
    "content.IMPRESSION": "content.impression",
    "content.QUARTILE": "content.quartile",
    "content.VIEW": "content.view",
    createPlayer: parent => new Player(parent),
    createPlayerAsync: parent => new Player(parent),
    createVPAIDPlayer: parent => new Player(parent),
    destroyAll() {},
    extension() {},
    getContext() {},
    player: parent => new Player(parent),
    playerInceptionTime() {
      return { undefined: 1620149827713 };
    },
    registerExtra(a, b, c) {
      extras.push([a, b, c]);
    },
    version: () => "21.1.313",
  };

  window.vidible = Shimmer.proxy(vidible);

  for (const jsonp of getJSONPVideoPlacements()) {
    const player = new Player(jsonp);
    const { placeholder } = playerData.get(player);
    jsonp.parentNode.insertBefore(placeholder, jsonp);
  }
}
