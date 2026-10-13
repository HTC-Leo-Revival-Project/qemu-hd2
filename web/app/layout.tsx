import type { Metadata, Viewport } from "next";
import { Roboto, Roboto_Mono } from "next/font/google";
import "./globals.css";

const roboto = Roboto({
  variable: "--font-roboto",
  subsets: ["latin"],
  weight: ["300", "400", "500", "700"],
});

const robotoMono = Roboto_Mono({
  variable: "--font-roboto-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  title: "HD2 Emulator",
  description: "Control panel for the HTC HD2 (htcleo) QEMU machine: live screen, hardware keys and boot configuration.",
  robots: { index: false, follow: false },
};

export const viewport: Viewport = {
  themeColor: "#000000",
  width: "device-width",
  initialScale: 1,
};

const RootLayout = ({ children }: LayoutProps<"/">) => (
  <html lang="en" className={`${roboto.variable} ${robotoMono.variable} h-full antialiased`}>
    <body className="h-full overflow-hidden">{children}</body>
  </html>
);

export default RootLayout;
