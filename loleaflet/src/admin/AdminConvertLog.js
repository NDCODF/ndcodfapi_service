/*
	Socket to be intialized on opening the history page in Admin console
*/
/* global $ nodejson2html Util AdminSocketBase */
/* eslint no-unused-vars:0 */
var AdminConvertLog = AdminSocketBase.extend({
	constructor: function(host) {
		this.base(host);
	},

	refreshHistory: function() {
		this.socket.send('convertlog');
	},

	onSocketOpen: function() {
		// Base class' onSocketOpen handles authentication
		this.base.call(this);

		var socketHistory = this;
		$('#refreshHistory').on('click', function () {
			return socketHistory.refreshHistory();
		});
		this.refreshHistory();
	},

	onSocketMessage: function(e) {
		var jsonObj;
		try {
			$('#json-doc').find('textarea').html(e.data);
			//$('#json-ex-doc').find('textarea').html(JSON.stringify(exdoc));
		} catch (e) {
			$('document').alert(e.message);
		}
	},

	onSocketClose: function() {

	}
});

Admin.ConvertLog = function(host) {
	return new AdminConvertLog(host);
};

