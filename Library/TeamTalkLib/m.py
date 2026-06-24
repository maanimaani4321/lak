import os

def gather_project_files_except_audio(folder_path, output_file):
    """
    تمام فایل‌ها را از یک پوشه و زیرپوشه‌های آن می‌خواند (به جز فایل‌های صوتی)
    و محتوا و آدرس نسبی هر فایل را در یک فایل خروجی ذخیره می‌کند.

    Args:
        folder_path (str): مسیر پوشه‌ای که حاوی فایل‌های پروژه است.
        output_file (str): نام فایل متنی که محتوا در آن ذخیره می‌شود.
    """
    # لیستی از پسوندهای فایل‌های صوتی که باید نادیده گرفته شوند
    audio_extensions = ('.mp3', '.wav', '.ogg', '.flac', '.aac', '.m4a', '.wma', '.aiff')

    if not os.path.isdir(folder_path):
        print(f"خطا: پوشه '{folder_path}' یافت نشد.")
        return

    try:
        with open(output_file, 'w', encoding='utf-8') as outfile:
            for root, dirs, files in os.walk(folder_path):
                for file in files:
                    # بررسی اینکه فایل صوتی نباشد
                    if not file.lower().endswith(audio_extensions):
                        file_path = os.path.join(root, file)
                        # ساخت آدرس نسبی نسبت به پوشه اصلی
                        relative_path = os.path.relpath(file_path, folder_path)

                        try:
                            with open(file_path, 'r', encoding='utf-8') as infile:
                                content = infile.read()
                                # نوشتن اطلاعات در فایل خروجی
                                outfile.write(f"--- فایل: {relative_path} ---\n")
                                outfile.write(content)
                                outfile.write("\n\n") # جدا کردن محتوای فایل‌ها
                        except Exception as e:
                            # برای فایل‌های غیر متنی (مثل فایل‌های باینری یا عکس) ممکن است خطا رخ دهد
                            # اگر این خطاها اهمیت دارند، می‌توانید نوع فایل را چک کنید یا
                            # فقط پیام خطا را ثبت کنید و از خواندن محتوا صرف نظر کنید.
                            outfile.write(f"--- خطا در خواندن فایل (احتمالاً غیر متنی): {relative_path} ---\n")
                            outfile.write(f"خطا: {e}\n\n")

        print(f"تمامی فایل‌های پروژه (به جز فایل‌های صوتی) با موفقیت در '{output_file}' ذخیره شدند.")

    except Exception as e:
        print(f"خطا در انجام عملیات: {e}")

if __name__ == "__main__":
    # نام پوشه پروژه را از کاربر دریافت کنید
    project_folder = input("لطفاً نام پوشه پروژه را وارد کنید: ")
    # نام فایل خروجی را مشخص کنید
    output_filename = "project_content_no_audio.txt"

    gather_project_files_except_audio(project_folder, output_filename)