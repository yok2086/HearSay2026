Shader "Unlit/MediaPipe/Overlay Mask Shader"
{
    Properties
    {
        _MainTex ("Main Texture", 2D) = "" {}
        _MaskTex ("Mask Texture", 2D) = "blue" {}
        _Width ("Mask Width", Int) = 0
        _Height ("Mask Height", Int) = 0
        _Threshold ("Threshold", Range(0.0, 1.0)) = 0.9
    }

    SubShader
    {
        Tags { "Queue"="Transparent" "RenderType"="Transparent" }

        ZWrite Off
        Blend SrcAlpha OneMinusSrcAlpha
        LOD 100

        Pass
        {
            CGPROGRAM

            #pragma vertex vert
            #pragma fragment frag

            #include "UnityCG.cginc"

            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;
            };

            struct v2f
            {
                float2 uv : TEXCOORD0;
                UNITY_FOG_COORDS(1)
                float4 vertex : SV_POSITION;
            };

            sampler2D _MainTex;
            float4 _MainTex_ST;

            sampler2D _MaskTex;

            int _Width;
            int _Height;
            float _Threshold;

            StructuredBuffer<float> _MaskBuffer;

            v2f vert(appdata v)
            {
                v2f o;

                o.vertex = UnityObjectToClipPos(v.vertex);
                o.uv = TRANSFORM_TEX(v.uv, _MainTex);

                UNITY_TRANSFER_FOG(o, o.vertex);

                return o;
            }

            float GetMask(float2 uv)
            {
                uv = saturate(uv);

                int x = min((int)(uv.x * _Width), _Width - 1);
                int y = min((int)(uv.y * _Height), _Height - 1);

                int idx = y * _Width + x;

                return _MaskBuffer[idx];
            }

            fixed4 frag(v2f i) : SV_Target
            {
                float2 pixel = float2(3.0 / _Width, 3.0 / _Height);

                float center = GetMask(i.uv);

                // Check the surrounding pixels.
                float top    = GetMask(i.uv + float2(0, pixel.y));
                float bottom = GetMask(i.uv - float2(0, pixel.y));
                float left   = GetMask(i.uv - float2(pixel.x, 0));
                float right  = GetMask(i.uv + float2(pixel.x, 0));

                float topLeft     = GetMask(i.uv + float2(-pixel.x, pixel.y));
                float topRight    = GetMask(i.uv + float2(pixel.x, pixel.y));
                float bottomLeft  = GetMask(i.uv + float2(-pixel.x, -pixel.y));
                float bottomRight = GetMask(i.uv + float2(pixel.x, -pixel.y));

                float centerPerson = step(_Threshold, center);

                float neighborPerson = max(
                    max(max(top, bottom), max(left, right)),
                    max(max(topLeft, topRight), max(bottomLeft, bottomRight))
                );

                neighborPerson = step(_Threshold, neighborPerson);

                // Only draw where the mask changes from background to person.
                float outline = neighborPerson * (1.0 - centerPerson);

                // Neon yellow.
                fixed4 outlineColor = fixed4(1.0, 1.0, 0.0, outline);

                UNITY_APPLY_FOG(i.fogCoord, outlineColor);

                return outlineColor;
            }

            ENDCG
        }
    }
}