# Face recognition: who is it?

The face finder (FOMO) tells *where* a face is. A second Edge Impulse model tells *who* it is.
Both models come with the firmware. You teach the F1 new people from your phone on the live preview page:
it learns them on the board, with no cloud and no Edge Impulse account, and keeps the list in its own flash.

![Live view on a phone](images/preview-phone.png)

*The preview page on a phone: a registered person in cyan with the match score, the people list below.
The photo is a public-domain NASA portrait of Eileen Collins.*

## How it works

1. FOMO finds the faces in the 96x96 grayscale image, as before (~160 ms).
2. The F1 cuts the best face out of the full-resolution colour frame and shrinks it to 96x96.
3. **MobileFaceNet** turns that picture into a *fingerprint*: 256 numbers. Two pictures of the same person
   give similar fingerprints, two different people don't (~2.4 s on the ESP32-S3).
4. The F1 compares it with the fingerprints of everyone registered. If the best match scores at least
   **0.45** (`face_id::kMatch`), that's who it is; otherwise "unknown".

Registering someone is the F1's on-board training: it takes 5 pictures, computes their fingerprints and
stores the average. The model itself stays the same, so learning a new person takes about 15 seconds and
never needs a computer.

Recognition runs when a face appears and then every 10 s while it stays, and only once somebody is registered.
LTE events then carry the name:

```json
{"device":"f1","event":"face","name":"Eileen Collins","similarity":0.63,"confidence":0.87,"faces":1,"inference_ms":161,"uptime_s":33,"csq":26}
```

## 1. Check it on the board

At boot the F1 tests the model on public-domain NASA portraits. Eileen Collins must match another photo of herself,
and nobody else:

```
I (7534) inference: self-test Eileen Collins (other photo) PASS  (similarity to Collins 0.75, 2353 ms)
I (9908) inference: self-test Neil Armstrong               PASS  (similarity to Collins 0.02, 2357 ms)
I (12286) inference: self-test Sally Ride                   PASS  (similarity to Collins 0.26, 2361 ms)
```

## 2. Register people

Turn on the [live preview](deploy-to-f1.md#5-live-preview-on-your-phone), join the F1's Wi-Fi, open
`http://192.168.4.1`, type a name and press **Register**. Stand alone, about 50 cm from the webcam, in good light,
and move your head a little between the 5 pictures. Registering the same name again adds more pictures to that person.
**Rename** and **Delete** are next to each name.

| LED | Meaning |
|---|---|
| Cyan | Someone you registered |
| Magenta | A face, unknown or not checked yet |

## 3. Start over

Flashing the board (`pio run -t upload`) erases the whole flash first, so the F1 forgets everyone it has learned.
Firmware flashed some other way does the same: on its first boot, a new firmware wipes the list
(`people: new firmware: forgetting all registered people` in the log). To remove one person without flashing,
use **Delete** on the page.

## Good to know

- **Distance matters.** FOMO finds face centres, not sizes, so the F1 always cuts out the same square (half the image
  height). Register at the distance people will usually stand from the camera.
- **One face at a time.** Only the best face is recognised. Other faces are still counted.
- **Accuracy.** On 30 people from the LFW benchmark, 88 to 90 of 90 test photos were matched to the right person.
  That's good enough for a demo or a welcome screen, but **not for security** (a photo can fool it).
- **Privacy.** Fingerprints are biometric data. They stay in the F1's `people` flash partition and never go over LTE,
  only the name does. **Delete** removes them, and flashing the board removes all of them.
- **The model.** NXP's int8 [MobileFaceNet](https://huggingface.co/nxp/mobilefacenet-imx), brought into Edge Impulse
  with *Bring your own model* and exported as a C++ library. The project is public:
  [F1 face recognition - MobileFaceNet](https://studio.edgeimpulse.com/public/1161859/latest). See also [How it works](how-it-works.md#where-the-models-come-from).
- **Licences.** MobileFaceNet: Apache-2.0 (NXP) / MIT (original). Test portraits: NASA, public domain.
