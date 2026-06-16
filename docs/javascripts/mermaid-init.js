/*
 * Mermaid 本地渲染初始化
 * 规避 Material 主题运行时从 unpkg CDN 动态加载 mermaid.js (国内访问不稳定/失败)
 * 改为加载本地 mermaid.min.js, 由本脚本自主渲染 class="mmd" 的图表元素
 * 支持 Material 暗色模式与 instant navigation (document$)
 */
(function () {
  "use strict";

  function currentTheme() {
    var scheme = document.body.getAttribute("data-md-color-scheme");
    return scheme === "slate" ? "dark" : "default";
  }

  function render() {
    if (typeof window.mermaid === "undefined") return;

    // superFences 生成 <pre class="mmd"><code>...</code></pre>
    // 转换为 <div class="mmd"> 包裹纯文本, 避免 <pre>/<code> 影响 mermaid 布局
    var pres = document.querySelectorAll("pre.mmd");
    Array.prototype.forEach.call(pres, function (pre) {
      var div = document.createElement("div");
      div.className = "mmd";
      div.textContent = pre.textContent.replace(/^\n+|\n+$/g, "");
      pre.parentNode.replaceChild(div, pre);
    });

    var nodes = Array.prototype.slice.call(
      document.querySelectorAll("div.mmd")
    );
    if (nodes.length === 0) return;

    window.mermaid.initialize({
      startOnLoad: false,
      theme: currentTheme(),
      securityLevel: "loose",
      flowchart: { useMaxWidth: true },
      sequence: { useMaxWidth: true },
    });

    window.mermaid
      .run({ nodes: nodes })
      .catch(function (err) {
        console.warn("[mermaid] 渲染失败:", err);
      });
  }

  // Material 内容加载 / instant navigation 时重新渲染
  if (typeof document$ !== "undefined") {
    document$.subscribe(render);
  } else if (document.readyState !== "loading") {
    render();
  } else {
    document.addEventListener("DOMContentLoaded", render);
  }
})();
