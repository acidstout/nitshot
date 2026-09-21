# Nitshot – Benutzerhandbuch

Version 0.8 · © 2026 Rekow IT

Nitshot ist ein schneller Ersatz für das Windows Snipping Tool. Drücken
Sie **Win+Shift+S** oder **PrtScn** (Druck), wählen Sie aus, was Sie möchten – und
der Screenshot liegt im selben Moment in der Zwischenablage und als Datei auf der
Festplatte. Außerdem kann das Programm einen Bildschirmbereich als Video aufnehmen,
und auf einem HDR-Display bleibt der volle Helligkeitsumfang erhalten, statt
plattgedrückt zu werden.

- [Erste Schritte](#erste-schritte)
- [Einen Snip aufnehmen](#einen-snip-aufnehmen)
- [Wo Ihre Snips landen](#wo-ihre-snips-landen)
- [HDR-Screenshots](#hdr-screenshots)
- [Den Bildschirm aufnehmen](#den-bildschirm-aufnehmen)
- [Das Tray-Symbol](#das-tray-symbol)
- [Einstellungen](#einstellungen)
- [Sprachen](#sprachen)
- [Fehlerbehebung](#fehlerbehebung)

---

## Erste Schritte

**Voraussetzungen:** Windows 10 oder Windows 11, 64 Bit.

Starten Sie `Nitshot.exe`. Es öffnet sich kein Fenster; das Programm
sitzt als kleines Symbol im Infobereich der Taskleiste (unten rechts). Ab dann gilt:

| Tasten | Was passiert |
|---|---|
| **Win+Shift+S** | öffnet das Capture-Overlay |
| **PrtScn** | öffnet das Capture-Overlay (oder nimmt sofort den ganzen Bildschirm auf, wenn Sie das so eingestellt haben) |
| **Alt+PrtScn** | unverändert – weiterhin die Windows-eigene Funktion „aktives Fenster kopieren“ |

Das Windows Snipping Tool bleibt installiert. Solange Nitshot läuft,
übernimmt es diese Tasten; beenden Sie es, gehören die Tastenkürzel sofort wieder
Windows. Dauerhaft verändert wird nichts.

Damit das Programm mit Windows startet, setzen Sie im Tray-Menü oder in den
Einstellungen den Haken bei **Mit Windows starten**.

---

## Einen Snip aufnehmen

Drücken Sie **Win+Shift+S**. Der Bildschirm friert ein, wird abgedunkelt, und oben
in der Mitte erscheint eine Werkzeugleiste:

| Schaltfläche | Taste | Modus |
|---|---|---|
| Rectangle | `1` | Rechteck aufziehen; der Snip wird beim Loslassen aufgenommen |
| Freeform | `2` | beliebige Form zeichnen; alles außerhalb der Umrandung wird transparent |
| Window | `3` | auf ein Fenster zeigen – es wird hervorgehoben – und klicken |
| Fullscreen | `4` | nimmt sofort alle Bildschirme auf |
| Record | | einen Bereich aufziehen und stattdessen **ein Video aufnehmen** – siehe [Aufnahme](#den-bildschirm-aufnehmen) |
| ✕ | `Esc` | abbrechen |

Auch ein **Rechtsklick** bricht ab.

Das Overlay merkt sich den zuletzt benutzten Modus und öffnet beim nächsten Mal
darin. Ausnahme ist Fullscreen: Das ist eine einmalige Aktion, der nächste Snip
öffnet wieder im vorherigen Modus.

Weil der Bildschirm im Moment des Tastendrucks eingefroren wird, lassen sich auch
Menüs, Tooltips und alles andere festhalten, was sonst verschwindet, sobald man die
Maus bewegt.

Bei mehreren Monitoren deckt das Overlay alle ab, und eine Auswahl darf über
Monitorgrenzen hinweg gehen.

---

## Wo Ihre Snips landen

Standardmäßig wird jeder Snip

- **in die Zwischenablage kopiert** – direkt in eine Mail, einen Chat oder ein
  Dokument einfügen;
- **als PNG gespeichert** in `Bilder\Nitshot`, mit einem Namen wie
  `Screenshot 2026-09-21 143012.png` (Datum und Uhrzeit des Snips).

Eine Benachrichtigung in der Ecke zeigt, wohin die Datei gespeichert wurde. Über
**Screenshot-Ordner öffnen** im Tray-Menü gelangen Sie direkt dorthin.

Freeform-Snips behalten in der PNG-Datei ihre transparente Umgebung. Programme, die
keine Transparenz verstehen, erhalten die Form auf weißem statt auf schwarzem
Hintergrund.

Beide Ziele lassen sich einzeln abschalten, der Ordner ändern und die
Benachrichtigung ausschalten – siehe [Einstellungen](#allgemein).

---

## HDR-Screenshots

Ist auf einem Display **HDR** eingeschaltet (Windows nennt das „erweiterte
Farben“), erkennt Nitshot das selbst und nimmt in vollem HDR auf:

- der Snip wird als **`.jxr`**-Datei gespeichert (JPEG XR – dasselbe Format, das
  die Xbox Game Bar verwendet; die Windows-Fotos-App öffnet es), mit allen
  Lichtern;
- daneben landet eine normale **`.png`**-Kopie für alles, was kein HDR versteht;
- die **Zwischenablage** erhält immer ein normales (SDR-)Bild, weil Windows kein
  HDR-Format für die Zwischenablage kennt.

Die normale 8-Bit-Kopie muss sich entscheiden: Helle Lichter werden entweder
schlicht weiß (Standard – normale Inhalte sehen exakt so aus wie auf dem
Bildschirm), oder sie werden zusammengestaucht, sodass Details sichtbar bleiben –
dafür wird Weiß ein klein wenig grau. Das ist die Option **Lichter in der
8-Bit-Kopie erhalten**.

Ohne HDR-Display spielt das alles keine Rolle, und Snips sind gewöhnliche PNG-Dateien.

---

## Den Bildschirm aufnehmen

1. **Win+Shift+S** drücken und in der Werkzeugleiste auf **Record** klicken.
2. Den Bereich aufziehen, der aufgenommen werden soll. Die Aufnahme beginnt sofort.
3. Ein roter Rahmen markiert den Bereich; eine kleine Leiste zeigt die Laufzeit und
   eine **Stop**-Schaltfläche. Beide liegen knapp außerhalb des Bereichs, sind also
   nicht im Video, und Sie können innerhalb des Bereichs ganz normal weiterarbeiten.
4. Beenden mit **Stop** oder erneut mit **Win+Shift+S** bzw. **PrtScn** – während
   einer Aufnahme bedeutet das Tastenkürzel „stopp“.

Das Video wird als **MP4** in `Videos\Nitshot` gespeichert, mit einem
Namen wie `Recording 2026-09-21 143012.mp4`, und eine Benachrichtigung meldet das.

**Ton:** entweder das, was der PC gerade abspielt (*Systemklang*), das *Mikrofon*
oder gar keiner. Jeweils eine Quelle.

**HDR:** Auf einem HDR-Display werden auch Videos in 10-Bit-HDR aufgenommen, Farben
und Lichter kommen also so heraus, wie sie auf dem Bildschirm aussahen. Dafür muss
die Grafikkarte HEVC (H.265) in 10 Bit kodieren können; fast jede aktuelle Karte
kann das. Kann Ihre es nicht oder schalten Sie die Option ab, wird in normalem SDR
aufgenommen – korrekt umgerechnet, nicht ausgewaschen.

**Besserer Ton:** Der Ton im MP4 ist komprimiert (AAC). Brauchen Sie das exakte
Original, schalten Sie die verlustfreie **.wav**-Option ein: Dann wird neben dem
Video eine `.wav` mit dem unveränderten Ton gespeichert. Siehe
[Aufnahme-Einstellungen](#aufnahme).

---

## Das Tray-Symbol

Ein **Doppelklick** auf das Symbol startet einen neuen Snip. Ein **Rechtsklick**
öffnet das Menü:

| Eintrag | |
|---|---|
| Neuer Snip | wie Win+Shift+S |
| Ganzen Bildschirm aufnehmen | nimmt sofort alle Bildschirme auf, ohne Overlay |
| Screenshot-Ordner öffnen | öffnet den Ordner im Explorer |
| Mit Windows starten | beim Anmelden automatisch starten |
| Einstellungen… | öffnet die Einstellungen |
| Info | Version und Kurzhilfe |
| Beenden | beendet das Programm; Win+Shift+S gehört wieder Windows |

---

## Einstellungen

Zu öffnen über **Tray-Symbol → Einstellungen…**. Das Fenster hat fünf Register.
Gewechselt wird per Maus oder mit **Strg+Tab** / **Strg+Shift+Tab** von überall im
Fenster.

Die drei Schaltflächen unten:

- **OK** speichert alles und schließt das Fenster.
- **Abbrechen** schließt, ohne die Änderungen zu speichern, die Sie seit dem Öffnen
  gemacht haben – bzw. seit dem letzten Klick auf Übernehmen.
- **Übernehmen** speichert und aktiviert Ihre Änderungen sofort, lässt das Fenster
  aber offen. Die Schaltfläche ist nur anklickbar, wenn sich etwas geändert hat.

Optionen, die nur zusammen mit einer anderen sinnvoll sind, sind ausgegraut,
solange diese andere aus ist.

### Allgemein

| Option | |
|---|---|
| Jeden Snip in die Zwischenablage kopieren | legt jeden Snip in die Zwischenablage |
| Dateien speichern | speichert jeden Snip zusätzlich als Datei |
| Bilder | Ordner für Snips. Mit **Durchsuchen…** wählen. Leer bedeutet `Bilder\Nitshot` |
| Videos | Ordner für Aufnahmen. Leer bedeutet `Videos\Nitshot`. Aufnahmen werden immer gespeichert, auch wenn *Dateien speichern* aus ist |
| Desktop abdunkeln um … Prozent | wie dunkel der Bildschirm hinter dem Overlay wird (0 = gar nicht) |
| Benachrichtigung anzeigen, wenn ein Snip gespeichert wurde | die Meldung in der Ecke nach jedem Snip |
| Sprache | *Automatisch* folgt der Windows-Anzeigesprache. Eine neue Auswahl stellt das Fenster sofort um, damit Sie lesen können, was Sie auswählen |
| Mit Windows starten | beim Anmelden automatisch starten |

### HDR

Die oberste Zeile zeigt, ob gerade ein Display im HDR-Modus läuft und wie hell
Windows dort normales (SDR-)Weiß darstellt.

| Option | |
|---|---|
| HDR-Snips als .jxr speichern | HDR-Snips als echte HDR-Dateien speichern |
| …und eine SDR-Kopie als .png daneben | zusätzlich eine normale PNG-Kopie speichern |
| Lichter in der 8-Bit-Kopie erhalten | helle Bereiche in der normalen Kopie zusammenstauchen, statt sie bei Weiß abzuschneiden |
| Qualität | JPEG-XR-Qualität, 1–100. **90** ist optisch nicht zu unterscheiden und schnell. **100** ist echt verlustfrei, kann für einen ganzen 4K-Bildschirm aber rund 20 Sekunden und den dreifachen Platz kosten |
| In HDR aufnehmen, wenn das Display in HDR läuft | auf einem HDR-Display Videos in 10-Bit-HDR aufnehmen |
| HDR ignorieren, 8-Bit aufnehmen | jedes Display wie ein normales behandeln, für Snips und Aufnahmen |

### Aufnahme

| Option | |
|---|---|
| Ton | *Kein*, *Systemklang* (was der PC abspielt) oder *Mikrofon* |
| Bilder pro Sekunde | 5 bis 60; 30 ist ein guter Standardwert |
| Encoder | welcher Video-Encoder benutzt wird. *Automatisch* wählt den besten, den Ihre Grafikkarte anbietet. Aufgeführt sind nur Encoder, die auf Ihrem PC erfolgreich getestet wurden; scheitert der gewählte, wird der nächstbeste genommen |
| Ton als verlustfreie .wav neben der .mp4 speichern | behält eine exakte Kopie des Tons als `.wav`-Datei |
| …und daraus den Ton der .mp4 erzeugen (nur die .mp4 bleibt) | der Ton des MP4 wird nach der Aufnahme aus der `.wav` erzeugt, danach wird die `.wav` gelöscht. Übrig bleibt eine Datei |

### Tastenkürzel

| Option | |
|---|---|
| Win+Shift+S übernehmen | Win+Shift+S öffnet Nitshot statt des Windows Snipping Tools |
| PrtScn übernehmen | dasselbe für die Druck-Taste |
| PrtScn nimmt den ganzen Bildschirm ohne Overlay auf | PrtScn speichert sofort alle Bildschirme, ohne Auswahl |

### Erweitert

| Option | |
|---|---|
| Immer GDI-Aufnahme verwenden | ein älteres, langsameres Aufnahmeverfahren. Nur nützlich, wenn Snips bei ungewöhnlichen Display-Konstellationen schwarz oder falsch herauskommen |
| Diagnose-Log | schreibt eine Protokolldatei zur Fehlersuche (siehe unten) |

Das Einstellungsfenster öffnet sich in der Bildschirmmitte. Verschieben Sie es,
öffnet es sich beim nächsten Mal an dieser Stelle.

---

## Sprachen

Englisch und Deutsch sind enthalten. Standardmäßig verwendet das Programm die
Windows-Anzeigesprache, und Englisch, wo es keine Übersetzung gibt.

**Eine eigene Sprache hinzufügen** geht ohne Programmierung:

1. Den Ordner `langs` neben `Nitshot.exe` öffnen.
2. `en.ini` kopieren und die Kopie nach dem Sprachkürzel benennen, z. B. `fr.ini`.
3. Den Text hinter jedem `=` übersetzen und die Datei als UTF-8 speichern (der
   Standard im Editor).
4. Die Einstellungen öffnen – die neue Sprache steht bereits in der Liste.

Noch nicht übersetzte Zeilen bleiben einfach Englisch, eine halb fertige
Übersetzung funktioniert also schon. Platzhalter wie `%1` und `%2` müssen erhalten
bleiben, dürfen im Satz aber verschoben werden. Begriffe wie *Snip*, *HDR*,
*PrtScn* und Dateiendungen bleiben in jeder Sprache gleich.

Ist der Programmordner schreibgeschützt, legen Sie die Datei stattdessen in
`%APPDATA%\Nitshot\langs\` ab.

---

## Fehlerbehebung

**Win+Shift+S öffnet das Windows Snipping Tool.**
Prüfen Sie, ob Nitshot läuft (Tray-Symbol) und *Win+Shift+S übernehmen*
angehakt ist. Solange ein Programm, das **als Administrator** läuft, den Fokus hat,
lässt Windows normale Programme die Tastatur nicht sehen – klicken Sie zuerst in ein
normales Fenster oder benutzen Sie das Tray-Symbol.

**Auf dem Sperrbildschirm oder während des Bildschirmschoners passiert nichts.**
Das ist so gewollt: Windows hält diese Bildschirme vom Desktop getrennt.

**Ein HDR-Video wirkt im Player blass oder gräulich.**
Die Datei ist in Ordnung; der Player rechnet sie schlecht um. In MPC-HC / MPC-BE mit
dem MPC Video Renderer die Option *HDR in SDR umwandeln* abschalten oder die
Display-Helligkeit auf die echte Spitzenhelligkeit Ihres Monitors setzen.

**Eine Aufnahme klingt etwas anders als das Original.**
Mit *Systemklang* enthält die Aufnahme, was Windows an die Lautsprecher schickt –
einschließlich Klangeffekten oder Equalizer des Audiotreibers. Spielt man diese
Aufnahme über dieselben Effekte ab, wirken sie doppelt. Für eine neutrale Aufnahme
in den Windows-Soundeinstellungen die „Audioverbesserungen“ des Wiedergabegeräts
abschalten.

**Ein Snip ist schwarz.**
Unter Erweitert *Immer GDI-Aufnahme verwenden* ausprobieren. Das betrifft vor allem
Remote-Desktops, virtuelle Displays und ungewöhnliche Grafikadapter.

**„Nichts gespeichert – bitte Einstellungen prüfen.“**
Sowohl *Jeden Snip in die Zwischenablage kopieren* als auch *Dateien speichern* sind
ausgeschaltet.

**Ein Problem melden.**
Unter Erweitert *Diagnose-Log* einschalten, das Problem nachstellen und die Datei
`%APPDATA%\Nitshot\Nitshot.log` schicken. Sie enthält nur
technische Angaben, keine Bilder.

**Wo werden die Einstellungen gespeichert?**
In `%APPDATA%\Nitshot\settings.ini`. Löschen setzt alles auf die
Standardwerte zurück.
