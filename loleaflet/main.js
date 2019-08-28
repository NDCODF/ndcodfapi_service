// If not debug, don't print anything on the console
// except in tile debug mode (Ctrl-Shift-Alt-d)
console.log2 = console.log;
if (loleaflet_logging !== 'true') {
	var methods = ['warn', 'info', 'debug', 'trace', 'log', 'assert', 'time', 'timeEnd'];
	for (var i = 0; i < methods.length; i++) {
		console[methods[i]] = function() {};
	}
}

var brandProductName = 'OxOffice Online';
global.brandProductName = brandProductName;

// Include our main css file
require('./main.css');

var $ = require('jquery');
global.$ = global.jQuery = $;

require('smartmenus');
require('jquery-ui');
require('jquery-contextmenu');
require('timeago');
global.Autolinker = require('autolinker');
// FIXME: would be good to remove w2ui script tags and require
// like other modules. problem is that w2ui doesn't export
// its global variables for a module, so following doesn't work
// This also leads to toolbar.js being included
//global.w2ui = require('./3rdparty/w2ui/w2ui');

global._ = function (string) {
	return string.toLocaleString();
};
require('json-js/json2');
require('l10n-for-node');
require('select2');
require('evol-colorpicker');
require('malihu-custom-scrollbar-plugin')($);

var vex = require('vex-js');
vex.dialog = require('vex-js/js/vex.dialog.js');
vex.defaultOptions.className = 'vex-theme-plain';
global.vex = vex;

var errorMessages = require('./dist/errormessages');

function getParameterByName(name) {
    name = name.replace(/[\[]/, "\\[").replace(/[\]]/, "\\]");
    var regex = new RegExp("[\\?&]" + name + "=([^&#]*)"),
    results = regex.exec(location.search);
    return results === null ? "" : results[1].replace(/\+/g, " ");
}
var lang = getParameterByName('lang');
if (lang) {
    String.locale = lang;
}
String.locale = String.locale.replace('zh_TW', 'zh-TW');

var L = require('loleaflet');
require('./dist/plugins/draw-0.2.4/dist/leaflet.draw.js');

var wopiSrc = getParameterByName('WOPISrc');

if (wopiSrc !== '') {
	var wopiParams = { 'access_token': access_token, 'access_token_ttl': access_token_ttl };
}

var filePath = getParameterByName('file_path');
if (!filePath)
{
    if (!getParameterByName('docid'))
           filePath = 'http://' + location.hostname + '/' + getParameterByName('docid');
}
var title = getParameterByName('title');
title = title === '' ? 'Loading...' : title;
if (getParameterByName('file_path')) {
    // 先解碼再拆
    title = decodeURIComponent(filePath);
    title = title.substring(title.lastIndexOf('/')+1);
}

var permission = 'edit';
if (filePath)
    if (getParameterByName("permission"))
	permission = getParameterByName("permission");
if (getParameterByName('preview') == '1') {
    permission = 'readonly';
}

var timestamp = getParameterByName('timestamp');
// Shows close button if non-zero value provided
var closebutton = getParameterByName('closebutton');
// Shows revision history file menu option
var revHistoryEnabled = getParameterByName('revisionhistory');
// Should the document go inactive or not
var alwaysActive = getParameterByName('alwaysactive');
// Loleaflet Debug mode
var debugMode = getParameterByName('debug');
if (getParameterByName('docid') === '') {
	if (wopiSrc === '' && filePath === '') {
	    vex.dialog.alert(errorMessages.wrongwopisrc);
	}
}
if (host === 'ws://' || host === 'wss://') {
    vex.dialog.alert(errorMessages.emptyhosturl);
}

// loleaflet.js accesses these globals
// TODO: Get rid of these globals
global.closebutton = closebutton;
global.revHistoryEnabled = revHistoryEnabled;
global.title = title;
global.errorMessages = errorMessages;
var docURL, docParams;
var isWopi = false;
if (wopiSrc != '') {
	docURL = decodeURIComponent(wopiSrc);
	docParams = wopiParams;
	isWopi = true;
} else {
    docURL = filePath;
	docParams = {};
}

if (!getParameterByName('rdid')) {
    docParams.source = getParameterByName('source');
    docParams.owner = getParameterByName('owner');
    docParams.docid = getParameterByName('docid');
}
else {
    docParams.rdid = getParameterByName('rdid');
    docURL = 'rdid=' + docParams.rdid;
}

document.title = title;
var map = L.map('map', {
	server: host,
	doc: docURL,
	docParams: docParams,
	permission: permission,
	timestamp: timestamp,
	documentContainer: 'document-container',
	debug: debugMode,
	wopi: isWopi,
	alwaysActive: alwaysActive,
	idleTimeoutSecs: idleTimeoutSecs,  // Dim when user is idle.
	outOfFocusTimeoutSecs: outOfFocusTimeoutSecs // Dim after switching tabs.
});
// toolbar.js (loaded in <script> tag accesses map as global variable,
// so expose it
global.map = map;

////// Controls /////
map.addControl(L.control.scroll());
if (permission === 'edit')
{
    map.addControl(L.control.dialog());
    map.addControl(L.control.partsPreview());
    map.addControl(L.control.tabs());
    map.addControl(L.control.columnHeader());
    map.addControl(L.control.rowHeader());
    map.addControl(L.control.contextMenu());
    map.addControl(L.control.menubar());
}
if (permission === 'view' || permission === 'readonly')
{
    map.addControl(L.control.tabs());
    map.addControl(L.control.columnHeader());
    map.addControl(L.control.rowHeader());
    map.addControl(L.control.menubar());
}

window.addEventListener('beforeunload', function () {
	if (global.map && global.map._socket) {
		global.map._socket.close();
	}
});
//require('./dist/toolbar/toolbar');

/*
 * 接收事件，觸發 download as
 */
function revMsg(event)
{
    console.log('cmd msg');

    if (event.data != 'cmd')
	return;

    var origin = event.origin || event.originalEvent.origin; // For Chrome, the o

    var format = 'pdf';
    // remove the extension if any
    var fileName = title.substr(0, title.lastIndexOf('.')) || title;
    // check if it is empty
    fileName = fileName === '' ? 'document' : fileName;
    map.downloadAs(fileName + '.' + format, format);

    return;
}
window.addEventListener('message', revMsg, false);
