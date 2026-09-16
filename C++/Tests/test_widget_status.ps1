$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '../Utility/Qt/QOneGenWidget.cpp')
if ($source -match '\breport\(') { throw 'Free-form status messages must not return.' }
if ([regex]::Matches($source, 'status_->setText\(').Count -ne 1) {
    throw 'The status label must have a single state-only writer.'
}
$body = [regex]::Match($source, 'void QOneGenWidget::updateStatus\(\)\s*\{(?<body>[\s\S]*?)\n\}').Groups['body'].Value
if (!$body) { throw 'Missing centralized state writer.' }
$states = [regex]::Matches($body, 'QStringLiteral\("([^"]*)"\)') | ForEach-Object { $_.Groups[1].Value }
if (($states -join ',') -ne 'Live,Connected,Disconnected,Idle') {
    throw 'Only canonical device states may be shown in the statusbar.'
}
'OneGen widget status routing check passed.'
