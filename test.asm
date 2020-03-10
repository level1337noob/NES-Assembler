.mapper $00
.org $C000
.prgsize $01
.chrsize $01
.text
.reloc "@main"

; fix relocation table later
@main:
	jsr _test
_test:
	rts
.data
db $B0,$FF,$FF,$FE
