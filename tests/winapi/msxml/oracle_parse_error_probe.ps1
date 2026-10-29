$cases = @(
  @('unclosed_tag',        '<a><b></a>'),
  @('no_root',             ''),
  @('junk_at_root',        'hello'),
  @('two_roots',           '<a/><b/>'),
  @('missing_quote',       '<a x=1/>'),
  @('missing_equals',      '<a x/>'),
  @('bad_start_name',      '<1a/>'),
  @('unterminated_tag',    '<a'),
  @('undefined_entity',    '<a>&nope;</a>'),
  @('missing_semicolon',   '<a>&amp</a>'),
  @('comment_syntax',      '<a><!-- x --- y --></a>'),
  @('bad_char_in_string',  "<a x='b<c'/>"),
  @('mismatched_close',    '<a></b>'),
  @('expecting_tag_end',   '<a b="c" d/>'),
  @('cdata_unterminated',  '<a><![CDATA[x</a>'),
  @('xmldecl_syntax',      '<?xml version=1.0?><a/>'),
  @('invalid_hex',         '<a>&#xZZ;</a>'),
  @('multiple_colons',     '<a:b:c/>')
)
foreach ($c in $cases) {
  $d = New-Object -ComObject Msxml2.DOMDocument.6.0
  $d.async = $false
  $null = $d.loadXML($c[1])
  $e = $d.parseError
  $code = $e.errorCode
  $hex = "0x{0:X8}" -f ($code -band 0xFFFFFFFFL)
  Write-Output ("{0}`t{1}`t{2}`tline={3}`tpos={4}`tfilepos={5}`t{6}" -f $c[0], $code, $hex, $e.line, $e.linepos, $e.filepos, ($e.reason -replace "`r?`n"," "))
}
