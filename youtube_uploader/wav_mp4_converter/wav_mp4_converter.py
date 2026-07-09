import os
import pickle
from time import sleep
import numpy as np
if not hasattr(np, 'complex'):
    np.complex = complex
import datetime
import random
import string  # Add for random string generation


# seed for update alerts
random.seed(16617436638245474392486)


# config load/setup
def config():
    min_seed = random.randint(1000000000000000, 99999999999999999)
    signature = random.randint(min_seed, (min_seed*2)-1+random.randint(9999,999999))
    if os.path.exists("conf.kal"):
        with open("conf.kal", "rb") as file:
            return pickle.load(file), signature
    else:
        temp = {"converted": [],
                "signature": signature}
        config_save(temp)


# config save
def config_save(conf):
    with open("conf.kal", "wb") as file:
        pickle.dump(conf, file)


# get wav files in dir
def get_all_wav(path):
    files = []
    paths = os.listdir(path)
    print(paths)
    for file in paths:
        if file.endswith(".wav"):
            try:
                files.append(str(file)+"||"+str(os.path.getmtime(path+file)))
            except Exception as e:
                print(f"The following file could not be processed: {str(file)}\n{e}")
    return files


# get newest wav
def get_newest_wav(wavs):
    return [i for i in wavs if wavs[i] == max(wavs.values())][0]


# get duration of wav
def wav_duration(path):
    import wave
    import contextlib
    try:
        with contextlib.closing(wave.open(path, 'r')) as f:
            frames = f.getnframes()
            rate = f.getframerate()
            duration = frames / float(rate)
            return duration
    except Exception as e:
        print(f"Error calculating duration for {path}: {e}")
        return 0


# merge wav and mp4
# ffmpeg -i "videoFile.mp4" -i "audioFile.mp3" -shortest outPutFile.mp4
# ffmpeg -i input.mp4 -ss 00:05:20 -t 00:10:00 -c:v copy -c:a copy output1.mp4
def merge(mp, wp, full_path):
    name = gen_name(conf_sig)
    os.system("ffmpeg -i \"{}\" -i \"{}\" -c:v copy -c:a aac -map 0:v:0 -map 1:a:0 \"{}\" -y".format(mp,
                                                                                                  full_path+wp,
                                                                                                  full_path+wp.replace(".wav",
                                                                                                             f"{name}_ToCut.mp4")))
    os.system("ffmpeg -i \"{}\" -ss 00:00:00 -t {} -c:v copy -c:a copy \"{}\" -y".format(
        full_path+wp.replace(".wav", f"{name}_ToCut.mp4"),
        str(datetime.timedelta(seconds=wav_duration(full_path+wp))),
        full_path+wp.replace(".wav", ".mp4")))    # hardcoded destination folder 
    os.replace(full_path+wp.replace(".wav", ".mp4"), "D:/[Library]/[Video]/[Works]/[Uploads]/"+wp.replace(".wav", ".mp4"))
    os.remove(full_path+wp.replace(".wav", f"{name}_ToCut.mp4"))


# naming convention
def gen_name(seed):
    # Generate a random 4-character string using letters and digits
    return ''.join(random.choices(string.ascii_letters + string.digits, k=4))


# together now
wav_path = "D:/[Library]/[Audio]/[Works]/"  # --> replace these with the appropriate paths
mp4_path = "D:/[Library]/[Video]/[Templates]/[YouTube template].mp4"

sleep(99)
conf, conf_sig = config()

wavs = get_all_wav(wav_path)
print("Wav files retrieved: {}".format(", ".join(wavs)))
for wav in wavs:
    print(wav)
    filename = wav.split("||")[0]
    if wav not in conf["converted"]:
        print(f"Merging {filename}")
        merge(mp4_path, filename, wav_path)
        conf["converted"].append(wav)
    else:
        print(f"Skipping {filename}")
config_save(conf)
