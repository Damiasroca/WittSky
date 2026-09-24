sidebar();

const ajax = function (option) {
  let init = {
    method: "GET",
    url: "",
    data: {},
    contentType: true, //false时为formdata
    success: function () {},
    error: function () {},
  };
  //合并参数
  Object.assign(init, option);

  init.method = init.method.toLocaleUpperCase();

  let xhr = new XMLHttpRequest();
  xhr.timeout = 70000; //超时
  // 超时回调
  xhr.ontimeout = function () {
    toast({
      txt: "network timeout",
      type: "error",
    });
    location.href = "./login.html";
  };
  xhr.responseType = "json";
  // 启动并发送一个请求
  if (init.method == "GET") {
    //参数转换
    let params = formatParams(init.data); // init.data请求的数据
    xhr.open("get", init.url + "?" + params, true);
    xhr.send();
  } else if (init.method == "POST") {
    xhr.open("post", init.url, true);
    // 设置表单提交时的内容类型
    // Content-type数据请求的格式
    if (init.contentType) {
      xhr.setRequestHeader("Content-type", "application/json");
      xhr.send(JSON.stringify(init.data));
    } else {
      xhr.send(init.data);
    }
  }
  xhr.onreadystatechange = function () {
    if (xhr.readyState == 4) {
      let status = xhr.status;
      if ((status >= 200 && status < 300) || status == 304) {
        init.success && init.success(xhr.response);
      } else {
        init.error && init.error(status);
      }
    }
  };
};

// 格式化请求参数
function formatParams(data) {
  let arr = [];
  for (let name in data) {
    arr.push(encodeURIComponent(name) + "=" + encodeURIComponent(data[name]));
  }
  return arr.join("&");
}

function removeError(_this) {
  //清除error class
  _this.classList.remove("error");
}


function sidebar() {
  if (!document.querySelector("#sidebar")) return;
  document.querySelector("#sidebar").innerHTML = `
        <a class="sidebar-a" href="./status.html">Status</a>
        <a class="sidebar-a" href="./localNetwork.html">Network</a>
        <a class="sidebar-a" href="./capture.html">Capture</a>
        <a class="sidebar-a" href="./skystats.html">Sky</a>
        <a class="sidebar-a" href="./video.html">Camera</a>
        <a class="sidebar-a" href="./system.html">System</a>
			`;
  let here = (location.pathname.split("/").pop() || "").toLowerCase();
  document.querySelectorAll(".sidebar-a").forEach(function (a) {
    let href = (a.getAttribute("href") || "").replace("./", "").toLowerCase();
    if (href && here === href) a.classList.add("sidebar-a-on");
  });
  if (localStorage.getItem("version")) {
    document
      .querySelector("#sidebar")
      .insertAdjacentHTML(
        "beforeend",
        `<p id="sidebar-version">${localStorage.getItem("version")}</p>`
      );
  }
}

function fmtUptime(s) {
  s = Math.floor(Number(s) || 0);
  if (s < 0) s = 0;
  let d = Math.floor(s / 86400);
  s %= 86400;
  let h = Math.floor(s / 3600);
  s %= 3600;
  let m = Math.floor(s / 60);
  let sec = s % 60;
  if (d) return d + "d " + h + "h " + m + "m";
  if (h) return h + "h " + m + "m";
  return m + "m " + sec + "s";
}

function fmtAge(s) {
  if (s === null || s === undefined || s < 0) return "never";
  s = Math.floor(s);
  if (s < 60) return s + "s ago";
  if (s < 3600) return Math.floor(s / 60) + "m ago";
  return Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m ago";
}

function fmtBytes(n) {
  n = Number(n) || 0;
  if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
  if (n >= 1024) return Math.round(n / 1024) + " KB";
  return n + " B";
}

function healthUploadText(res) {
  if (!res || res.last_upload_age_s < 0) return "never";
  let st = res.last_upload_ok == 1 ? "ok" : "fail";
  let msg = res.last_upload_msg ? " — " + res.last_upload_msg : "";
  return fmtAge(res.last_upload_age_s) + " " + st + msg;
}

function fillHealthStrip(res) {
  let el = document.querySelector("#status-strip");
  if (!el || !res) return;
  let rssi =
    res.rssi === null || res.rssi === undefined
      ? "—"
      : res.rssi + " dBm (" + (res.rssi_label || "") + ")";
  let up = healthUploadText(res);
  let camOk = res.camera_ok == 1;
  let rssiEl = el.querySelector("#strip-rssi");
  rssiEl.innerText = rssi;
  let rssiTone = "err";
  if (res.rssi_label === "good") rssiTone = "ok";
  else if (res.rssi_label === "fair") rssiTone = "warn";
  else if (res.rssi === null || res.rssi === undefined) rssiTone = "err";
  rssiEl.className = rssiTone;
  el.querySelector("#strip-up").innerText = fmtUptime(res.uptime_s);
  let upEl = el.querySelector("#strip-upload");
  upEl.innerText = up;
  upEl.className = res.last_upload_age_s < 0 ? "" : res.last_upload_ok == 1 ? "ok" : "err";
  let camEl = el.querySelector("#strip-cam");
  camEl.innerText = camOk ? "ok" : "down";
  camEl.className = camOk ? "ok" : "err";
}

function statusStrip() {
  if (!document.querySelector("#sidebar")) return;
  if (document.querySelector("#status-strip")) return;
  let content = document.querySelector("#content");
  if (!content) return;
  let el = document.createElement("div");
  el.id = "status-strip";
  el.className = "status-strip";
  el.style.display = "none";
  el.innerHTML =
    '<span>RSSI <b id="strip-rssi">—</b></span>' +
    '<span>Up <b id="strip-up">—</b></span>' +
    '<span>Upload <b id="strip-upload">—</b></span>' +
    '<span>Camera <b id="strip-cam">—</b></span>';
  content.insertBefore(el, content.firstChild);
  function poll() {
    ajax({
      url: "/get_health",
      success: function (res) {
        el.style.display = "flex";
        fillHealthStrip(res);
        if (typeof window.onHealth === "function") window.onHealth(res);
      },
      error: function (err) {
        if (err == 401) el.style.display = "none";
      },
    });
  }
  setTimeout(poll, 0);
  setInterval(poll, 15000);
}

statusStrip();
class Tip {
  constructor(option) {
    this.txt = option.txt;
    this.type = option.type;
    this.time = option.time || 3000;
    this.cls = ["success", "error"].includes(option.type) ? option.type : "info";
  }
  toast() {
    this.el = `<div class="model_tip tip-${this.cls}">${this.txt}</div>`;
    this.el_box = document.createElement("div");
    this.el_box.innerHTML = this.el;
    document.body.appendChild(this.el_box);
    setTimeout(function () {
      document.querySelector(".model_tip").style.opacity = 0;
      document.body.removeChild(
        document.querySelector(".model_tip").parentNode
      );
    }, this.time);
  }
}
function toast({ txt = "none", type, time }) {
  return new Tip({
    txt,
    type,
    time,
  }).toast();
}
class Loading {
  constructor() {}
  show(txt = "") {
    let template = document.createElement("div");
    template.id = "loading-template";
    template.innerHTML = `
		<div class="loading-box">
		<div id="loading-txt">${txt}</div>
			<div class="loading">
				<div></div>
				<div></div>
				<div></div>
			</div>
		</div>
	`;
    document.body.appendChild(template);
  }

  hide() {
    let loading = document.querySelector("#loading-template");
    document.body.removeChild(loading);
  }
}

class Modal {
  constructor(el) {
    this.elBox = document.getElementById(el);
  }
  show(txt = "") {
    let template = document.createElement("div");
    template.id = "modal";
    template.innerHTML = `
    <div id="modal">
    <div class="mask"></div>
    <div class="modal-body">
      <div class="txt-box">
        ${txt}
      </div>
      <div class="modal-btn">
        OK
      </div>
    </div>
  </div>
	`;
    this.elBox.appendChild(template);
    document.querySelector("#modal").addEventListener("click", (e) => {
      if (e.target.className === "modal-btn") {
        this.hide();
      }
    });
  }

  hide() {
    let modal = document.querySelector("#modal");
    this.elBox.removeChild(modal);
  }
}

let _keyStr = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
function utf8Encode(string) {
	//base64编码
	string = string.replace(/\r\n/g, '\n');
	var utftext = '';
	for (var n = 0; n < string.length; n++) {
		var c = string.charCodeAt(n);
		if (c < 128) {
			utftext += String.fromCharCode(c);
		} else if (c > 127 && c < 2048) {
			utftext += String.fromCharCode((c >> 6) | 192);
			utftext += String.fromCharCode((c & 63) | 128);
		} else {
			utftext += String.fromCharCode((c >> 12) | 224);
			utftext += String.fromCharCode(((c >> 6) & 63) | 128);
			utftext += String.fromCharCode((c & 63) | 128);
		}
	}
	return utftext;
}

function baseCode(input) {
	//base64编码
	var output = '';
	var chr1, chr2, chr3, enc1, enc2, enc3, enc4;
	var i = 0;
	input = utf8Encode(input);
	while (i < input.length) {
		chr1 = input.charCodeAt(i++);
		chr2 = input.charCodeAt(i++);
		chr3 = input.charCodeAt(i++);
		enc1 = chr1 >> 2;
		enc2 = ((chr1 & 3) << 4) | (chr2 >> 4);
		enc3 = ((chr2 & 15) << 2) | (chr3 >> 6);
		enc4 = chr3 & 63;
		if (isNaN(chr2)) {
			enc3 = enc4 = 64;
		} else if (isNaN(chr3)) {
			enc4 = 64;
		}
		output = output + _keyStr.charAt(enc1) + _keyStr.charAt(enc2) + _keyStr.charAt(enc3) + _keyStr.charAt(enc4);
	}
	return output;
}


function utf8_decode(utftext) {
  //base64解密
  var string = "";
  var i = 0;
  var c = 0;
  var c2 = 0;
  var c3 = 0;
  while (i < utftext.length) {
    c = utftext.charCodeAt(i);
    if (c < 128) {
      string += String.fromCharCode(c);
      i++;
    } else if (c > 191 && c < 224) {
      c2 = utftext.charCodeAt(i + 1);
      string += String.fromCharCode(((c & 31) << 6) | (c2 & 63));
      i += 2;
    } else {
      c2 = utftext.charCodeAt(i + 1);
      c3 = utftext.charCodeAt(i + 2);
      string += String.fromCharCode(
        ((c & 15) << 12) | ((c2 & 63) << 6) | (c3 & 63)
      );
      i += 3;
    }
  }
  return string;
}

function baseDecode(input) {
  //base64解密
  var output = "";
  var chr1, chr2, chr3;
  var enc1, enc2, enc3, enc4;
  var i = 0;
  input = input.replace(/[^A-Za-z0-9+\\=]/g, "");
  while (i < input.length) {
    enc1 = _keyStr.indexOf(input.charAt(i++));
    enc2 = _keyStr.indexOf(input.charAt(i++));
    enc3 = _keyStr.indexOf(input.charAt(i++));
    enc4 = _keyStr.indexOf(input.charAt(i++));
    chr1 = (enc1 << 2) | (enc2 >> 4);
    chr2 = ((enc2 & 15) << 4) | (enc3 >> 2);
    chr3 = ((enc3 & 3) << 6) | enc4;
    output = output + String.fromCharCode(chr1);
    if (enc3 != 64) {
      output = output + String.fromCharCode(chr2);
    }
    if (enc4 != 64) {
      output = output + String.fromCharCode(chr3);
    }
  }
  output = utf8_decode(output);
  return output;
}