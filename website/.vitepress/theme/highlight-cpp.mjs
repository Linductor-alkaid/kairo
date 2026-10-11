// 首页示例只有几段短 C++，用轻量 tokenizer 着色，避免在主题里引入运行时高亮器。

const keywords = new Set([
  'auto', 'const', 'return', 'if', 'else', 'void', 'int', 'bool', 'true', 'false',
  'nullptr', 'std', 'kairo', 'struct', 'class', 'for', 'while'
])

const tokenPattern = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')|(\b\d+\b)|([A-Za-z_]\w*)(?=\s*(?:\(|\{|<))|([A-Za-z_]\w*)/g

function escapeHtml(text) {
  return text.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
}

export function highlightCpp(source) {
  let html = ''
  let last = 0
  for (const match of source.matchAll(tokenPattern)) {
    html += escapeHtml(source.slice(last, match.index))
    last = match.index + match[0].length
    const [text, comment, string, number, call, word] = match
    let kind = null
    if (comment) kind = 'comment'
    else if (string) kind = 'string'
    else if (number) kind = 'number'
    else if (keywords.has(call ?? word)) kind = 'keyword'
    else if (call) kind = 'call'
    else if (/^[A-Z]/.test(word)) kind = 'type'
    html += kind ? `<span class="tk-${kind}">${escapeHtml(text)}</span>` : escapeHtml(text)
  }
  return html + escapeHtml(source.slice(last))
}
