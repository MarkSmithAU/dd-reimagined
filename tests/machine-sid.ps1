# Windows writes the SID of the machine (or domain) that formats an NTFS
# volume into its security descriptors: a file's group is <machine>\None
# (S-1-5-21-a-b-c-513).  The NTFS fixtures must not carry the SID of the
# machine that made them, so make-ntfs-fixtures.ps1 replaces it with
# $PlaceholderSid before compressing, and sparse-tests.ps1 puts the local
# machine's SID back when it unpacks them.  dd's NTFS driver never reads
# the descriptors; the swap keeps them looking as Windows wrote them.
#   . (Join-Path $PSScriptRoot 'machine-sid.ps1')

$PlaceholderSid = 'S-1-5-21-1111111111-2222222222-3333333333'

# The local machine's SID: every local account's SID is it plus a RID
function Get-MachineSid {
    (Get-LocalUser | Select-Object -First 1).SID.AccountDomainSid.Value
}

# The SIDs Windows may have written for files the current user created:
# the machine's, and the user's own domain if it is a domain account
function Get-LocalSids {
    @(Get-MachineSid) + [Security.Principal.WindowsIdentity]::GetCurrent().User.AccountDomainSid.Value |
        Where-Object { $_ } | Select-Object -Unique
}

# The part of a domain SID (S-1-5-21-a-b-c) that is the machine's: a, b and
# c, 12 bytes.  Only these are matched: NTFS's update sequence fixup can
# replace a SID's header bytes where it crosses a sector boundary, and 96
# random bits cannot match anything else by chance.
function SidParts([string]$domainSid) {
    $sid = New-Object Security.Principal.SecurityIdentifier $domainSid
    $b = New-Object byte[] $sid.BinaryLength
    $sid.GetBinaryForm($b, 0)
    if ($b.Length -ne 24 -or $b[1] -ne 4 -or $b[8] -ne 21) { throw "$domainSid is not S-1-5-21-a-b-c" }
    ,[byte[]]$b[12..23]
}

# Replaces every SID under domain $from with the same SID under $to, in a
# file; returns how many were replaced
function Convert-Sid([string]$path, [string]$from, [string]$to) {
    $latin1 = [Text.Encoding]::GetEncoding(28591)       # one char per byte, both ways
    $f = $latin1.GetString((SidParts $from))
    $t = $latin1.GetString((SidParts $to))
    $s = $latin1.GetString([IO.File]::ReadAllBytes($path))
    $n = 0
    for ($i = $s.IndexOf($f, [StringComparison]::Ordinal); $i -ge 0; $i = $s.IndexOf($f, $i + 1, [StringComparison]::Ordinal)) { $n++ }
    if ($n) { [IO.File]::WriteAllBytes($path, $latin1.GetBytes($s.Replace($f, $t))) }
    $n
}
