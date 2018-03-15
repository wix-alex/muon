const {app, BrowserWindow, ipcMain, webContents, session, protocol} = require('electron')
const path = require('path')
const browserActions = require('./browser-actions')
const assert = require('assert')
const deepEqual = require('../../common/deep-equal')

// List of currently active background pages by extensionId
var backgroundPages = {}

// List of events registered for background pages by extensionId
var backgroundPageEvents = {}

// List of current active tabs
var tabs = {}

const TAB_ID_NONE = -1

function debounce (fn, bufferInterval, ...args) {
  let timeout
  return (...args2) => {
    clearTimeout(timeout)
    let a = args || []
    if (args2 && args2.constructor === Array) {
      a = a.concat(args2)
    }
    timeout = setTimeout(fn.apply.bind(fn, this, a), bufferInterval)
  }
}

var getResourceURL = function (extensionId, path) {
  path = String(path)
  if (!path.length || path[0] != '/')
    path = '/' + path
  return 'chrome-extension://' + extensionId + path
}

process.on('EXTENSION_READY_INTERNAL', (installInfo) => {
  process.emit('extension-ready', installInfo)
  let browserAction = installInfo.manifest.browser_action
  if (browserAction) {
    let details = {
      title: browserAction.default_title,
      path: browserAction.default_icon,
      popup: browserAction.default_popup
    }
    browserActions.setDefaultBrowserAction(installInfo.id, details)
    addBackgroundPageEvent(installInfo.id, 'chrome-browser-action-clicked')
    process.emit('chrome-browser-action-registered', installInfo.id, details)
  }
  addBackgroundPageEvent(installInfo.id, 'chrome-context-menus-clicked')
})

// TODO(bridiver) move these into modules
// background pages
var addBackgroundPage = function (extensionId, backgroundPage) {
  backgroundPages[extensionId] = backgroundPages[extensionId] || []
  backgroundPages[extensionId].push(backgroundPage)
  process.emit('background-page-loaded', extensionId, backgroundPage)
}

var addBackgroundPageEvent = function (extensionId, event) {
  backgroundPageEvents[event] = backgroundPageEvents[event] || []
  if (backgroundPageEvents[event].indexOf(extensionId) === -1) {
    backgroundPageEvents[event].push(extensionId)
  }
}

var sendToBackgroundPages = function (extensionId, session, event) {
  if (!backgroundPageEvents[event] || !session) { return }

  var pages = []
  if (extensionId === 'all') {
    pages = backgroundPageEvents[event].reduce(
      (curr, id) => curr = curr.concat(backgroundPages[id] || []), [])
  } else {
    pages = backgroundPages[extensionId] || []
  }

  var args = [].slice.call(arguments, 2)
  pages.forEach(function (backgroundPage) {
    try {
      var sendToAll = (typeof (session) === 'string' && session === 'all')
      // otherwise, only send to background pages in the same browser context

      if (sendToAll || backgroundPage.session.equal(session)) {
        backgroundPage.send.apply(backgroundPage, args)
      }
    } catch (e) {
      console.error('Could not send to background page: ' + e)
    }
  })
}

var createBackgroundPage = function (tab) {
  var extensionId = tab.getURL().match(/chrome-extension:\/\/([^\/]*)/)[1]
  let id = tab.getId()
  addBackgroundPage(extensionId, tab)
  tab.on('destroyed', function () {
    process.emit('background-page-destroyed', extensionId, id)
    var index = backgroundPages[extensionId].indexOf(tab)
    if (index > -1) {
      backgroundPages[extensionId].splice(index, 1)
    }
  })
}

const createTabValue = function (tab) {
  const tabId = tab.getId()

  if (tabs[tabId]) {
    return tabId
  }

  tabs[tabId] = {}
  tabs[tabId].webContents = tab
  tabs[tabId].tabValue = getTabValue(tabId)
  sendToBackgroundPages('all', getSessionForTab(tabId), 'chrome-tabs-created', tabs[tabId].tabValue)
  return tabId
}

// chrome.tabs
var getSessionForTab = function (tabId) {
  let tab = getWebContentsForTab(tabId)
  return tab && !tab.isDestroyed() && tab.session
}

var getWebContentsForTab = function (tabId) {
  return tabs[tabId] && tabs[tabId].webContents
}

var getTabValue = function (tabId) {
  var tabContents = getWebContentsForTab(tabId)
  return tabContents && !tabContents.isDestroyed() && tabContents.tabValue()
}

var getActiveTab = function (windowId) {
  return tabsQuery({windowId: windowId || -2, active: true})[0]
}

var getWindowIdForTab = function (tabId) {
  const tabValue = getTabValue(tabId)
  return tabValue ? tabValue.windowId : -1
}

var getTabsForWindow = function (windowId) {
  return tabsQuery({windowId})
}

var updateWindow = function (windowId, updateInfo) {
  let win = BrowserWindow.fromId(windowId)

  if (win) {
    if (updateInfo.focused) {
      win.focus()
    }

    if (updateInfo.left || updateInfo.top ||
      updateInfo.width || updateInfo.height) {
      let bounds = win.getBounds()
      bounds.x = updateInfo.left || bounds.x
      bounds.y = updateInfo.top || bounds.y
      bounds.width = updateInfo.width || bounds.width
      bounds.height = updateInfo.height || bounds.height
      win.setBounds(bounds)
    }

    switch (updateInfo.state) {
      case 'minimized':
        win.minimize()
        break
      case 'maximized':
        win.maximize()
        break
      case 'fullscreen':
        win.setFullScreen(true)
        break
    }

    return windowInfo(win, false)
  } else {
    console.warn('chrome.windows.update could not find windowId ' + windowId)
    return {}
  }
}

var removeWindow = function (windowId, cb) {
  let error
  let win = BrowserWindow.fromId(windowId)
  if (win) {
    win.close()
  } else {
    error = 'chrome.windows.remove could not find windowId ' + windowId
    console.warn(error)
  }
  cb(error)
}

const createTab = function (createProperties, cb) {
  let windowId = createProperties.windowId || -2
  let win = null
  let error = null

  if (windowId === -2) {
    win = BrowserWindow.getActiveWindow() || BrowserWindow.getAllWindows()[0]
    if (!win) {
      error = 'No current window'
    }
  } else {
    win = BrowserWindow.fromId(windowId)
    if (!win) {
      error = 'Window not found'
    }
  }

  let ses = session.defaultSession
  if (!error && createProperties.openerTabId) {
    opener = webContents.fromTabID(createProperties.openerTabId)
    if (!opener) {
      error = 'No tab found'
    } else {
      ses = opener.session
    }
  }

  if (!error && createProperties.partition) {
    // createProperties.partition always takes precendence
    ses = session.fromPartition(createProperties.partition, {
      parent_partition: createProperties.parent_partition,
      isolated_storage: createProperties.isolated_storage,
      tor_proxy: createProperties.tor_proxy,
      tor_path: createProperties.tor_path
    })
    // don't pass the partition info through
    delete createProperties.partition
    delete createProperties.parent_partition
  }

  if (error) {
    console.error(error)
    return cb(null, error)
  }

  createProperties.userGesture = true

  try {
    // handle url, active, index and pinned in browser-laptop
    webContents.createTab(
      win.webContents,
      ses,
      createProperties,
      (tab) => {
        if (tab) {
          cb(tab)
        } else {
          cb(null, 'An unexpected error occurred')
        }
      }
    )
  } catch (e) {
    console.error(e)
    cb(null, 'An unexpected error occurred: ' + e.message)
  }
}

const removeTabs = function (tabIds) {
  for (let tabId of tabIds) {
    var tabContents = getWebContentsForTab(tabId)
    if (tabContents) {
      if (!tabContents.isDestroyed()) {
        tabContents.close(tabContents)
      }
    } else {
      return 'No tab with id: ' + tabId
    }
  }
}

const updateTab = function (tabId, updateProperties) {
  var tabContents = getWebContentsForTab(tabId)
  if (!tabContents)
    return

  if (updateProperties.url) {
    tabContents.loadURL(updateProperties.url)
  }
  if (updateProperties.active || updateProperties.selected || updateProperties.highlighted) {
    tabContents.setActive(true)
  }
}

const chromeTabsUpdated = function (tabId) {
  const tab = tabs[tabId]
  if (!tab) {
    return
  }

  let oldTabInfo = tab.tabValue
  if (!oldTabInfo) {
    return
  }

  let tabValue = getTabValue(tabId)
  tabs[tabId].tabValue = tabValue
  let changeInfo = {}

  for (var key in tabValue) {
    if (!deepEqual(tabValue[key], oldTabInfo[key])) {
      changeInfo[key] = tabValue[key]
    }
  }

  if (Object.keys(changeInfo).length > 0) {
    if (changeInfo.active) {
      sendToBackgroundPages('all', getSessionForTab(tabId), 'chrome-tabs-activated', tabId, {tabId: tabId, windowId: tabValue.windowId})
      process.emit('chrome-tabs-activated', tabId, {tabId: tabId, windowId: tabValue.windowId})
    }
    sendToBackgroundPages('all', getSessionForTab(tabId), 'chrome-tabs-updated', tabId, changeInfo, tabValue)
    process.emit('chrome-tabs-updated', tabId, changeInfo, tabValue)
  }
}

const chromeTabsRemoved = function (tabId) {
  if (!tabs[tabId]) {
    return
  }

  let windowId = getWindowIdForTab(tabId)
  let session = getSessionForTab(tabId)
  delete tabs[tabId]
  sendToBackgroundPages('all', session, 'chrome-tabs-removed', tabId, {
    windowId,
    isWindowClosing: windowId === -1 ? true : false
  })
  browserActions.onDestroyed(tabId)
  process.emit('chrome-tabs-removed', tabId, windowId)
}

Array.prototype.diff = function(a) {
  return this.filter(function(i) {return a.indexOf(i) < 0;});
};

const tabsQuery = function (queryInfo = {}, useCurrentWindowId = false) {
  var tabIds = Object.keys(tabs)

  // convert current window identifier to the actual current window id
  if (queryInfo.windowId === -2 || queryInfo.currentWindow === true) {
    delete queryInfo.currentWindow
    var focusedWindow = BrowserWindow.getFocusedWindow()
    if (focusedWindow) {
      queryInfo.windowId = focusedWindow.id
    }
  }

  var queryKeys = Object.keys(queryInfo)
  // get the values for all tabs
  var tabValues = tabIds.reduce((tabs, tabId) => {
    tabs[tabId] = getTabValue(tabId) || {}
    return tabs
  }, {})

  var result = []
  tabIds.forEach((tabId) => {
    // never add the master Brave container window
    if (tabValues[tabId].url.startsWith('chrome://brave')) {
      return
    }

    // delete tab from the list if any key doesn't match
    if (!queryKeys.map((queryKey) => (tabValues[tabId][queryKey] === queryInfo[queryKey])).includes(false)) {
      result.push(tabValues[tabId])
    }
  })

  return result
}

app.on('web-contents-created', function (event, tab) {
  if (tab.isBackgroundPage()) {
    createBackgroundPage(tab)
    return
  }

  var tabId = tab.getId()
  if (tabId === -1)
    return

  tab.on('pinned', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('page-title-updated', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('did-fail-load', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('did-fail-provisional-load', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('did-attach', function () {
    createTabValue(tab)
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('did-detach', () => {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('did-start-loading', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('did-stop-loading', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('did-start-navigation', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('did-finish-navigation', function () {
    chromeTabsUpdated(tabId)
  })
  tab.on('navigation-entry-commited', function (evt, url) {
    createTabValue(tab)
    chromeTabsUpdated(tabId)
  })
  tab.on('did-navigate', function (evt, url) {
    createTabValue(tab)
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('did-navigate-in-page', function (evt, url, isMainFrame) {
    chromeTabsUpdated(tabId)
  })
  tab.on('load-start', function (evt, url, isMainFrame, isErrorPage) {
    if (isMainFrame) {
      createTabValue(tab)
      chromeTabsUpdated(tabId)  // immediate
    }
  })
  tab.on('did-finish-load', function () {
    createTabValue(tab)
    chromeTabsUpdated(tabId)
  })
  tab.on('set-active', function (evt, active) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('tab-inserted-at', function (evt, index, foreground) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('tab-changed-at', function (evt, index) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('tab-moved', function (evt, fromIndex, toIndex) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('tab-selection-changed', function (evt) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('tab-closing-at', function (evt, index) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('tab-detached-at', function (evt, index) {
    chromeTabsUpdated(tabId)  // immediate
  })
  tab.on('will-destroy', function () {
    chromeTabsRemoved(tabId)
  })
  tab.on('destroyed', function () {
    chromeTabsRemoved(tabId)
  })
})

ipcMain.on('chrome-tabs-create', function (evt, responseId, createProperties) {
  const cb = (tab, error) => {
    if (!evt.sender.isDestroyed()) {
      evt.sender.send('chrome-tabs-create-response-' + responseId, tab.tabValue(), error)
    }
  }
  try {
    createTab(createProperties, cb)
  } catch (e) {
    cb(null, e.message)
  }
})

ipcMain.on('chrome-tabs-remove', function (evt, responseId, tabIds) {
  let senderTabId = evt.sender.getId()
  tabIds = Array.isArray(tabIds) ? tabIds : [tabIds]
  let error = removeTabs(tabIds)
  // only send the response if the sender tab was not removed
  if (tabIds.indexOf(senderTabId) === -1) {
    evt.sender.send('chrome-tabs-remove-response-' + responseId, error)
  }
})

ipcMain.on('chrome-tabs-get-current', function (evt, responseId) {
  var response = getTabValue(evt.sender.getId())
  evt.sender.send('chrome-tabs-get-current-response-' + responseId, response)
})

ipcMain.on('chrome-tabs-get', function (evt, responseId, tabId) {
  var response = getTabValue(tabId)
  evt.sender.send('chrome-tabs-get-response-' + responseId, response)
})

ipcMain.on('chrome-tabs-query', function (evt, responseId, queryInfo) {
  var response = tabsQuery(queryInfo)
  evt.sender.send('chrome-tabs-query-response-' + responseId, response)
})

ipcMain.on('chrome-tabs-update', function (evt, responseId, tabId, updateProperties) {
  var response = updateTab(tabId, updateProperties)
  evt.sender.send('chrome-tabs-update-response-' + responseId, response)
})

ipcMain.on('chrome-tabs-execute-script', function (evt, responseId, extensionId, tabId, details) {
  var tab = getWebContentsForTab(tabId)
  if (tab) {
    tab.executeScriptInTab(extensionId, details.code || '', details, (error, on_url, results) => {
      evt.sender.send('chrome-tabs-execute-script-response-' + responseId, error, on_url, results)
    })
  }
})

var tabEvents = ['updated', 'created', 'removed', 'activated']
tabEvents.forEach((event_name) => {
  ipcMain.on('register-chrome-tabs-' + event_name, function (evt, extensionId) {
    addBackgroundPageEvent(extensionId, 'chrome-tabs-' + event_name)
  })
})

ipcMain.on('register-protocol-string-handler', function (evt, scheme) {
  const sender = evt.sender
  if (evt.sender.isDestroyed()) {
    return
  }

  sender.once('will-destroy', () => {
    try {
      protocol.unregisterProtocol(scheme)
    } catch (e) {}
  })

  let counter = 0
  protocol.registerStringProtocol(scheme, (request, cb) => {
    const requestId = scheme + '-' + counter++
    if (sender.isDestroyed()) {
      cb('')
    } else {
      ipcMain.once('chrome-protocol-handled-' + requestId, (evt, data) => {
        cb(data)
      })
      sender.send('chrome-protocol-handler-' + scheme, request, requestId)
      // TODO(bridiver) - timeout??
    }
  })
})

ipcMain.on('register-chrome-window-focus', function (evt, extensionId) {
    addBackgroundPageEvent(extensionId, 'chrome-window-focus')
})

ipcMain.on('chrome-cookies-getall', function (evt, responseId, details) {
  const cb = (error, cookielist) => {
    if (!evt.sender.isDestroyed()) {
      evt.sender.send('chrome-cookies-getall-response-' + responseId, error, cookielist)
    }
  }

  evt.sender.session.cookies.getAll(details, cb)
})

ipcMain.on('chrome-cookies-get', function (evt, responseId, details) {
  const cb = (error, cookie) => {
    if (!evt.sender.isDestroyed()) {
      var first = cookie

      if (Array.isArray(cookie) && cookie.length) {
        first = cookie[0]
      }

      evt.sender.send('chrome-cookies-get-response-' + responseId, error, first)
    }
  }

  evt.sender.session.cookies.get(details, cb)
})

ipcMain.on('chrome-cookies-set', function (evt, responseId, details) {
  const cb = (error, cookie) => {
    if (!evt.sender.isDestroyed()) {
      evt.sender.send('chrome-cookies-set-response-' + responseId, error, cookie)
    }
  }

  evt.sender.session.cookies.set(details, cb)
})

app.on('browser-window-focus', function (event, browserWindow) {
  var forthcomingWindow = browserWindow
  var forthcomingId = (forthcomingWindow == null) ? -1 : forthcomingWindow.id

  // Only background pages have access to chrome.window, so this is enough
  // We do need to send this to all sessions though
  sendToBackgroundPages('all', 'all', 'chrome-window-focus', forthcomingId)
})

app.on('browser-window-blur', function (event, browserWindow) {
  // see https://github.com/electron/electron/issues/4942
  setImmediate(function () {
    var forthcomingWindow = BrowserWindow.getFocusedWindow()

    var forthcomingId = (forthcomingWindow == null) ? -1 : forthcomingWindow.id
    var previousId = browserWindow.isDestroyed() ? -1 : browserWindow.id

    var allWindowsBlurred = (forthcomingId === -1 && previousId !== -1)
    if (allWindowsBlurred) {
      var blurredResponse = -1
      sendToBackgroundPages('all', 'all', 'chrome-window-focus', blurredResponse)
    }
  })
})

// chrome.windows
var windowInfo = function (win, populateTabs) {
  var bounds = win.getBounds()
  return {
    // create psuedo-windows to handle this
    incognito: false, // TODO(bridiver)
    id: win.id,
    focused: win.isFocused(),
    top: bounds.y,
    left: bounds.x,
    width: bounds.width,
    height: bounds.height,
    alwaysOnTop: win.isAlwaysOnTop(),
    tabs: populateTabs ? getTabsForWindow(win.id) : null
  }
}

ipcMain.on('chrome-windows-create', function (evt, responseId, createData) {
  process.emit('chrome-windows-create-action', responseId, createData, evt)
})

ipcMain.on('chrome-windows-get', function (evt, responseId, windowId, getInfo) {
  var response = {
    id: -1,
    focused: false,
    incognito: false // TODO(bridiver)
  }
  if (getInfo && getInfo.windowTypes) {
    console.warn('getWindow with windowTypes not supported yet')
  }
  var win = BrowserWindow.fromId(windowId)
  if (win) {
    if (getInfo && getInfo.populate) {
      response = windowInfo(win, getInfo.populate)
    } else {
      response = windowInfo(win, false)
    }
  }

  evt.sender.send('chrome-windows-get-response-' + responseId,
    response)
})

ipcMain.on('chrome-windows-get-current', function (evt, responseId, getInfo) {
  var response = {
    id: -1,
    focused: false,
    incognito: false // TODO(bridiver)
  }
  if(getInfo && getInfo.windowTypes) {
    console.warn('getWindow with windowTypes not supported yet')
  }
  var focusedWindow = BrowserWindow.getFocusedWindow()
  if (focusedWindow) {
    response = windowInfo(focusedWindow, getInfo.populate)
  }

  evt.sender.send('chrome-windows-get-current-response-' + responseId,
    response)
})

ipcMain.on('chrome-windows-get-all', function (evt, responseId, getInfo) {
  if (getInfo && getInfo.windowTypes) {
    console.warn('getWindow with windowTypes not supported yet')
  }
  var response = BrowserWindow.getAllWindows().map((win) => {
    if (getInfo && getInfo.populate) {
      return windowInfo(win, getInfo.populate)
    } else {
      return windowInfo(win, false)
    }
  })

  evt.sender.send('chrome-windows-get-all-response-' + responseId,
    response)
})

ipcMain.on('chrome-windows-update', function (evt, responseId, windowId, updateInfo) {
  var response = updateWindow(windowId, updateInfo)
  evt.sender.send('chrome-windows-update-response-' + responseId, response)
})

ipcMain.on('chrome-windows-remove', function (evt, responseId, windowId) {
  const cb = (error) => {
    if (!evt.sender.isDestroyed()) {
      evt.sender.send('chrome-windows-remove-response-' + responseId, error)
    }
  }
  try {
    removeWindow(windowId, cb)
  } catch (e) {
    cb(null, e.message)
  }
})

// chrome.browserAction

ipcMain.on('chrome-browser-action-set-badge-background-color', function (evt, extensionId, details) {
  process.emit('chrome-browser-action-set-badge-background-color', extensionId, details)
  browserActions.setBadgeBackgroundColor(extensionId, details)
})

ipcMain.on('chrome-browser-action-get-background-color-', function (evt, responseId, details) {
  let result = browserActions.getBackgroundColor(extensionId)
  evt.sender.send('chrome-browser-action-get-background-color-response-' + responseId, result)
})

ipcMain.on('chrome-browser-action-set-icon', function (evt, responseId, extensionId, details) {
  process.emit('chrome-browser-action-set-icon', extensionId, details)
  evt.sender.send('chrome-browser-action-set-icon-response-' + responseId)
})

ipcMain.on('chrome-browser-action-get-icon-', function (evt, responseId, details) {
  let result = browserActions.getIcon(extensionId)
  evt.sender.send('chrome-browser-action-get-icon-response-' + responseId, result)
})

ipcMain.on('chrome-browser-action-set-badge-text', function (evt, extensionId, details) {
  process.emit('chrome-browser-action-set-badge-text', extensionId, details, details.tabId)
  browserActions.setBadgeText(extensionId, details)
})

ipcMain.on('chrome-browser-action-get-badge-text-', function (evt, responseId, details) {
  let result = browserActions.getBadgeText(extensionId)
  evt.sender.send('chrome-browser-action-get-badge-text-response-' + responseId, result)
})

ipcMain.on('chrome-browser-action-set-title', function (evt, extensionId, details) {
  process.emit('chrome-browser-action-set-title', extensionId, details)
  browserActions.setTitle(extensionId, details)
})

ipcMain.on('chrome-browser-action-get-title', function (evt, responseId, details) {
  let result = browserActions.getTitle(extensionId)
  evt.sender.send('chrome-browser-action-get-title-response-' + responseId, result)
})

ipcMain.on('chrome-browser-action-set-popup', function (evt, extensionId, details) {
  process.emit('chrome-browser-action-set-popup', extensionId, details)
  browserActions.setPopup(extensionId, details)
})

ipcMain.on('chrome-browser-action-get-popup', function (evt, responseId, details) {
  let result = browserActions.getPopup(extensionId, details)
  evt.sender.send('chrome-browser-action-get-popup-response-' + responseId, result)
})

ipcMain.on('chrome-browser-action-clicked', function (evt, extensionId, tabId, name, props) {
  let popup = browserActions.getPopup(extensionId, {tabId})
  if (popup) {
    process.emit('chrome-browser-action-popup', extensionId, tabId, name, getResourceURL(extensionId, popup), props)
  } else {
    let response = getTabValue(tabId)
    if (response) {
      sendToBackgroundPages(extensionId, getSessionForTab(tabId), 'chrome-browser-action-clicked', response)
    }
  }
})

// TODO(bridiver) - refactor this in browser-laptop and remove
// https://github.com/brave/browser-laptop/pull/3241/files#r75715202
ipcMain.on('autofill-selection-clicked', function (evt, tabId, value, frontend_id, index) {
  let tab = getWebContentsForTab(tabId)
  if (tab)
    tab.autofillSelect(value, frontend_id, index)
})

ipcMain.on('autofill-popup-hidden', function (evt, tabId) {
  let tab = getWebContentsForTab(tabId)
  if (tab)
    tab.autofillPopupHidden()
})

ipcMain.on('chrome-context-menus-remove-all', function (evt, responseId, extensionId) {
  process.emit('chrome-context-menus-remove-all', extensionId)
  evt.sender.send('chrome-context-menus-remove-all-response-' + responseId)
})

ipcMain.on('chrome-context-menus-create', function (evt, responseId, extensionId, menuItemId, properties, icon) {
  process.emit('chrome-context-menus-create', extensionId, menuItemId, properties, icon)
  evt.sender.send('chrome-context-menus-create-response-' + responseId)
})

process.on('chrome-context-menus-clicked', function (extensionId, tabId, info) {
  let response = getTabValue(tabId)
  if (response) {
    sendToBackgroundPages(extensionId, getSessionForTab(tabId), 'chrome-context-menus-clicked', info, response)
  }
})

exports.createTab = createTab
exports.tabsQuery = tabsQuery
exports.getWebContentsForTab = getWebContentsForTab
