# mpc-vst-rattler

**Rattler** (Arbeitstitel) – paraphoner Schwarm-Synthesizer als VST2-Plugin für die Akai Force, nachempfunden nach dem Eowave Quadrantid Swarm.

**Status:** Phase 1 (Grundstimme) läuft auf der Force. Phase 2 (Charakter) umgesetzt, Test auf der Force steht aus. Phase 3 offen. Grundlage: Handbuch `swarm_manual_online_v2.pdf`.

## Ziel

Eine Stimme für Drones, Flächen und metallisch-perkussive Texturen, die auf der Force performant läuft und sich vollständig am Gerät bedienen lässt.

## Abgrenzung

- Kein Port: Die Original-Firmware ist nicht offen. Neubau nach Handbuch und Hörvergleich.
- Im Original sind Filter, VCA und Federhall analog, nur die VCOs digital. Hier wird alles digital modelliert, Ziel ist der Charakter.
- Kein Eowave-Branding, eigener Skin.

## Architektur laut Handbuch

Paraphon: 8 VCOs plus Percussion-Stimme teilen sich **eine** Filterkette, einen VCA und einen Hall.

```
Percussion ──┐
8 VCOs ──────┼─► Mixer ─► VCF 1 (12 dB, LP/HP) ─► VCF 2 (12 dB, LP) ─► VCA ─► Federhall ─► Out
Hall-Return ─┘   (3 Eingänge)                                              │
      ▲────────────────────────────────────────────────────────────────────┘
                              (Feedback über Mixer-Eingang 3)

Hüllkurve (AR/AD) ─► VCA und VCF 1
LFO ─► Slew ─► VCF 2
Sequencer (8 Steps) ─► VCO-Frequenz und Hüllkurven-Gate
```

**Festlegung:** Die Kette folgt dem Blockdiagramm (VCA vor Hall), damit die Hallfahne nicht abgeschnitten wird, wenn die Hüllkurve schließt. Der Handbuchtext nennt die umgekehrte Reihenfolge, sie ist über den Schalter `Rev Pre VCA` wählbar.

## Die 8 VCO-Modelle

| Nr. | Modell | Klangerzeugung | „Character" regelt |
|---|---|---|---|
| 1 | Organ | 8 Sinus | Wavefolder |
| 2 | Strings | 8 Sägezahn | Wavefolder |
| 3 | Drone | 8 Dreieck | Wavefolder |
| 4 | Reed | 8 × 2 VCOs, der zweite leicht verstimmt | Wavefolder |
| 5 | Metal | 8 Sinus plus 9. Oszillator als FM-Quelle | FM-Tiefe |
| 6 | Chiptune | 8 Rechteck mit PWM | PWM-Tiefe |
| 7 | Grains | 8 Granulargeneratoren auf Sinus-Wavelets | Anteil kurzes Delay |
| 8 | Noise | digitales, gestimmtes Rauschen | Anteil kurzes Delay |

## Spread

Stellt das Frequenzverhältnis der 8 VCOs ein, harmonisch bei Organ und Strings, unharmonisch bei allen anderen. Wirkt nur im Mono-Modus. Die Verhältnisse selbst nennt das Handbuch nicht, das Folgende ist eine **Annahme**:

- Reglerkurve: `Sc = S^3` (bis `S^4`, nach Gehör), mit S von 0 bis 1
- Harmonisch (Organ, Strings): `f_i = f0 · (1 + i · Sc)`, i = 0…7
- Unharmonisch (alle anderen): `f_i = f0 · (1 + Sc · i^1.4)`

| Reglerbereich | Wirkung |
|---|---|
| unteres Drittel | wenige Cent Abstand, Schwebung und Chorus |
| Mitte | Cluster, die Oszillatoren laufen auseinander |
| voll auf (harmonisch) | Obertöne 1–8: Grundton, Oktaven, Quinte, Terz |
| voll auf (unharmonisch) | gespreizte, glockige Teiltöne über rund vier Oktaven |

Der Verlauf ist stufenlos, ohne Sprünge beim Drehen. Summe der 8 VCOs mit 1/√8 normieren.

## Percussion

- Transientengenerator: weißes Rauschen plus Impuls, durch einen eigenen Bandpass
- Eigene exponentielle Decay-Hüllkurve, ausgelöst mit jedem Gate
- `Perc` regelt die Decay-Länge (wie im Handbuch), von wenigen Millisekunden bis etwa eine Sekunde
- `Perc Freq` verschiebt Bandpass-Mittenfrequenz und Tonhöhe des Impulses. **Erweiterung**, am Original nicht vorhanden

Die Klangerzeugung im Original ist nicht dokumentiert, das Modell ist eine Annahme.

## Spielmodi

- **Mono:** Eine Note setzt die Grundtonhöhe aller 8 VCOs, Spread fächert sie auf.
- **Poly:** Jeder der 8 VCOs hat eine eigene Tonhöhe und einen eigenen VCA, Spread ist ohne Funktion. Filter und Hüllkurve bleiben gemeinsam.
  *Abweichung vom Original:* Statt 8 Touch-Keys mit je einem Tonhöhen-Poti werden eingehende MIDI-Noten frei auf die 8 VCOs verteilt (bis 8 Noten gleichzeitig).
- **Sequencer:** 8 Steps mit Tonhöhe und Gate pro Step, Start/Stop, Reset auf Step 1.

## Parameter

| Gruppe | Parameter |
|---|---|
| VCO | Modell (1–8), Freq, Freq Mod, Spread, Character |
| Percussion | Perc (Decay-Länge), Perc Freq (Erweiterung) |
| Mixer | Voice Vol, Perc Vol, In (Hall-Feedback) |
| VCF 1 | Cutoff, Res, Mod (Hüllkurve), LP/HP |
| VCF 2 | Cutoff, Res, Mod (LFO) |
| Hüllkurve | Attack, Decay/Release, AD/AR |
| LFO | Speed, Shape (Dreieck, Sinus, Ramp down, Ramp up, Random 1–3, Seq), Slew |
| Federhall | Rev Input, Rev Level, Rev Pre VCA (Schalter) |
| Modus | Mono / Poly |
| Sequencer | 8 × Step-Tonhöhe, 8 × Gate, Clock (Teiler), Start |
| Global | Volume, Preset-Platz (1–32), LOAD, SAVE |

LFO-Shape 8 („Seq") gibt die 8 Step-Werte im LFO-Tempo aus, unabhängig von der Sequencer-Clock.

### MIDI-CC wie im Original

CC1 LFO Speed · CC2 Spread · CC3 Fold (Character) · CC4 Perc · CC5 Attack · CC6 Decay · CC7 Volume. Die CC-Werte werden zum Reglerwert addiert.

## Technischer Rahmen

- DPF-Plugin, Aufbau und Build nach Vorlage von `mpc-vst-rat` / `mpc-vst-carp2000`
- UID-Vorschlag: `Rttl`
- Build per GitHub Actions, Ziel: Force mit Firmware 3.9.1.0
- Skin mit festen Positionen und ausreichend großer Schrift (Lehre aus dem RAT-Skin)
- CPU: höchstens 16 Oszillatoren (Reed), zwei Filter, ein Hall – unabhängig von der Notenzahl, da paraphon

## Wiederverwendung

| Baustein | Quelle |
|---|---|
| Filter (Grundlage, plus Sättigung) | `mpc-vst-carp2000` |
| Federhall | `mpc-vst-carp2000`, Phase 3 |
| Clock und Sequencer-Logik | `mpc-vst-acid` (nach den Clock-Fixes) |
| 32 Speicherplätze mit LOAD/SAVE | `mpc-vst-acid` |

Die Preset-Verwaltung kommt von Anfang an nach Acid-Muster hinein.

## Fahrplan

### Phase 1 – Grundstimme

- Modelle Organ, Strings, Drone mit Spread und Wavefolder
- Mixer, VCF 1 und VCF 2 in Serie, VCA
- Hüllkurve AR/AD auf VCA und VCF 1
- Mono-Modus
- 32 Speicherplätze, LOAD/SAVE

**Fertig, wenn:** Plugin lädt auf der Force, spielt per Pads, Spread und Character sind deutlich hörbar, Presets erscheinen in der Liste und lassen sich umschalten.

### Phase 2 – Charakter

- Modelle Reed, Metal, Chiptune, Grains, Noise
- Percussion-Stimme
- LFO mit Shapes 1–7, Slew, Ziel VCF 2
- Freq Mod
- MIDI-CC 1–7

**Fertig, wenn:** Alle 8 Modelle klingen klar unterschiedlich und Character tut je Modell das, was die Tabelle sagt.

### Phase 3 – Ausbau

- Federhall mit Feedback in den Mixer
- Poly-Modus
- 8-Step-Sequencer, synchron zur Force-Clock, LFO-Shape „Seq"
- Skin-Feinschliff

**Fertig, wenn:** Sequencer läuft stabil zur Clock, Poly mit 8 Noten ohne Aussetzer, Feedback lässt sich ohne Pegelexplosion aufdrehen (Limiter im Feedback-Weg).

## Stand Phase 1 und 2

Umgesetzt in `vst/rattler_core.h` (Klang) und `vst/rattler_vst.cpp` (Plugin-Hülle, MIDI, Speicherplätze). Handgeschriebene VST2-Hülle ohne Wrapper wie beim carp 2000, Build über den gemeinsamen Workflow von `sd88me/mpc-vst-plugins`.

### Bedienseiten

| Seite | Regler 1–4 | Regler 5–8 |
|---|---|---|
| VOICE | Model, Freq (± 24 Halbtöne), Spread, Character | Voice Vol, Freq Mod, Freq Mod Source, Volume |
| PERC | Perc (Decay-Länge), Perc Freq, Perc Vol, Voice Vol | Attack, Decay/Release, Envelope (AR/AD), Volume |
| FILTER | VCF 1 Cutoff, VCF 1 Res, VCF 1 Env Mod, VCF 1 Type (LP/HP) | VCF 2 Cutoff, VCF 2 Res, VCF 2 LFO Mod, LFO Speed |
| LFO | Speed, Shape, Slew, VCF 2 LFO Mod | Freq Mod, Freq Mod Source |
| PRESET | Preset (1–32), LOAD, SAVE | |

Einige Regler liegen auf zwei Seiten, damit die Percussion mit ihrer Hüllkurve auf einer Seite spielbar ist.

### Festlegungen in Phase 1

- **Spread:** Formeln wie oben, Exponent 3. Die Konstanten `SPREAD_EXP` und `SPREAD_INH` stehen oben in `rattler_core.h` und lassen sich nach Gehör ändern. Spread geht nie ganz auf null (Rest: Bruchteil eines Cents), weil acht freilaufende Oszillatoren auf exakt gleicher Frequenz sich gegenseitig auslöschen könnten.
- **Wavefolder:** wirkt je Oszillator vor der Summe, Verstärkung 1- bis 8-fach.
- **Filter:** zwei 12-dB-State-Variable-Filter statt der 24-dB-Ladder des carp 2000 (die passt nicht zu 2 × 12 dB). Vom carp 2000 übernommen sind Sättigung und Rechenregeln. Bereich 20 Hz bis 18 kHz, Hüllkurve öffnet VCF 1 um bis zu 8 Oktaven.
- **Hüllkurve:** jede neue Taste löst sie aus (wie ein Gate am Original), Anschlagstärke bleibt ohne Wirkung. AD läuft auch bei gehaltener Taste ab.
- **Mono:** die zuletzt gedrückte Taste gilt, Pitch Bend ± 2 Halbtöne.
- **Speicherplätze:** 32 Plätze nach Acid-Muster, zugleich die VST-Programme (Preset-Liste der Force). Der Preset-Regler blättert nur, LOAD lädt, SAVE speichert. Plätze 1–8 sind mit Werksklängen belegt, bis SAVE sie überschreibt. Datei: `rattler_presets.txt` neben dem Plugin-Ordner, gemeinsam für alle Instanzen und Projekte.

### Festlegungen in Phase 2

- **Percussion:** gefiltertes Rauschen (Bandpass) plus kurzer Sinus-Ton auf der Bandpass-Frequenz, eigene Decay-Hüllkurve, ausgelöst mit jeder Taste. `Perc` 3 ms bis 1 s, `Perc Freq` 60 Hz bis 8 kHz. Die Tonhöhe folgt **nicht** der gespielten Note. Die Percussion läuft wie im Original durch Filter und VCA: Sie ist nur so lange zu hören, wie die Haupthüllkurve offen ist. Für reine Percussion `Voice Vol` auf 0 stellen (dann rechnen die VCOs auch nicht mit).
- **Freq Mod:** ein Tiefenregler (bis ± 4 Oktaven) mit Quellenwahl `LFO` / `ENV` / `DUAL`. Am Original fest am LFO, die Hüllkurve nur über Patchpunkte. Der LFO geht ungeglättet (vor dem Slew) in die Tonhöhe.
- **Reed:** je Stimme zwei Rechteck-Oszillatoren, der zweite rund 10 Cent höher.
- **Metal:** 9. Oszillator auf dem 2,76-Fachen der gespielten Note, Character regelt die FM-Tiefe.
- **Chiptune:** jeder der 8 Pulse hat eine eigene langsame Pulsbreiten-Modulation (0,3 bis 1,5 Hz), Character regelt die Tiefe.
- **Grains:** rund 30 Wavelets pro Sekunde und Generator, Länge zufällig, jedes vierte fällt aus.
- **Noise:** 8 Sample-&-Hold-Rauschquellen, getaktet mit dem 6-Fachen der jeweiligen VCO-Frequenz.
- **Kurzes Delay** (Grains, Noise): 30 ms, Feedback 50 %, Character regelt den Anteil.
- **LFO:** 0,05 bis 50 Hz. Random 1: neue Stufe pro Durchlauf. Random 2: gleitet zwischen Zufallswerten. Random 3: Stufen in unregelmäßigen Abständen. Slew bis 1 s. Shape „Seq" kommt mit dem Sequencer in Phase 3.
- **MIDI-CC 1–7:** wie im Original zum Reglerwert addiert, nicht gespeichert. Sendet die Force auf der Spur CC7 (Lautstärke), hebt das den Volume-Regler an.
- Die Konstanten zu Reed, Metal, Grains, Noise und Delay stehen oben in `rattler_core.h`.
- Plätze 9–16 haben Werksklänge zu Phase 2 (drei reine Percussion-Klänge, die neuen Modelle), solange dort nichts gespeichert ist.

### Bauen und testen

GitHub → Actions → „VST release (draft)" → Run workflow (Version z. B. `0.1.0`, `dry_run` an für einen reinen Build). Der Workflow baut `rattler.so` für armhf, erzeugt den Skin und lässt `vst/test.sh` laufen (Host-Test mit ASan/UBSan plus CPU-Messung).

## Offene Fragen

Das Handbuch lässt Folgendes offen, hier hilft nur Hören am Original oder an Demos:

1. **Spread-Verhältnisse:** Formeln oben sind Annahmen. Kurve und Exponent am Original oder an Demos abgleichen.
2. **Percussion-Stimme:** Modell oben ist eine Annahme. Folgt die Tonhöhe im Original der gespielten Note?
3. **Grains:** Wavelet-Länge und Dichte.
4. **Kurzes Delay** bei Grains und Noise: Delayzeit, Feedback ja/nein.
5. **Random 1–3:** Worin sich die drei Zufallsformen des LFO unterscheiden.
6. **Metal:** Frequenz des 9. Oszillators relativ zu den anderen.
7. ~~**Freq Mod:** Welche Quelle im Plugin?~~ Entschieden: Quellenwahl `LFO` / `ENV` / `DUAL` mit einem Tiefenregler.
8. **Federhall:** Reicht der carp-2000-Hall, oder braucht es mehr „Scheppern"?

## Referenzen

- Eowave Quadrantid Swarm, User Manual (`swarm_manual_online_v2.pdf`)
- Demovideos als Hörreferenz
